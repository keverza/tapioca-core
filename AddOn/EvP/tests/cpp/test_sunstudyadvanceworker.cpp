#include "SunStudy/SunStudyAdvanceWorker.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>

using namespace evp::sunstudy;

namespace {

struct Gate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future ().share ();
    std::atomic<size_t> calls { 0 };
    std::atomic<bool> throwNext { false };
    std::thread::id thread;
};

class ControlledTraversal final : public ITraversal {
  public:
    explicit ControlledTraversal (std::shared_ptr<Gate> gate) : gate_ (std::move (gate))
    {
    }
    void OccludeDirectional (const double* origins, size_t count, const double*, double, double, uint8_t* out,
                             size_t) const override
    {
        if (gate_->calls.fetch_add (1) == 0) {
            gate_->thread = std::this_thread::get_id ();
            gate_->entered.set_value ();
            gate_->released.wait ();
        }
        if (gate_->throwNext.exchange (false))
            throw std::runtime_error ("test traversal failure");
        for (size_t sample = 0; sample < count; ++sample)
            out[sample] = origins[sample * 3] < 0.0 ? 1u : 0u;
    }
    void OccludeRays (const OcclusionRay*, size_t count, uint8_t* out, size_t) const override
    {
        std::fill (out, out + count, uint8_t { 0 });
    }
    uint64_t SceneVersion () const override
    {
        return 31;
    }

  private:
    std::shared_ptr<Gate> gate_;
};

std::unique_ptr<StudyRecord> Record (const std::shared_ptr<const ITraversal>& traversal)
{
    auto record = std::make_unique<StudyRecord> ();
    record->positions = { 1.0, 0.0, 0.0 };
    record->normals = { 0.0, 0.0, 1.0 };
    record->traversal = traversal;
    std::vector<SunStep> steps (3);
    for (size_t step = 0; step < steps.size (); ++step) {
        steps[step].time = { static_cast<int> (12 + step), 0 };
        steps[step].altitudeDegrees = 90.0;
        steps[step].direction[2] = 1.0;
    }
    record->series = SunSeries::FromSteps (steps, 60);
    record->session.Sync ({ 31, record->series.Version (), 1 }, record->series, record->Samples ());
    return record;
}

struct PausedStudy {
    std::shared_ptr<Gate> gate = std::make_shared<Gate> ();
    std::shared_ptr<const ITraversal> traversal = std::make_shared<ControlledTraversal> (gate);
    std::future<void> entered = gate->entered.get_future ();
    SunStudyAdvanceWorker worker;
    AdvanceRequest request;
    std::string error;
    bool released = false;

    PausedStudy ()
    {
        SunStudyStore::Get ().Clear ();
        request.studyId = SunStudyStore::Get ().Insert (Record (traversal));
        request.sessionGeneration = 7;
        request.runGeneration = 11;
    }
    ~PausedStudy ()
    {
        Release ();
        worker.Shutdown ();
        SunStudyStore::Get ().Clear ();
    }
    void Release ()
    {
        if (!released) {
            released = true;
            gate->release.set_value ();
        }
    }
    bool Started ()
    {
        return entered.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
    }
    bool WaitIdle ()
    {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while (worker.Busy ()) {
            if (std::chrono::steady_clock::now () >= deadline)
                return false;
            std::this_thread::yield ();
        }
        return true;
    }
    bool WaitResult (AdvanceCompletion& completion)
    {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while (!worker.Poll (completion)) {
            if (std::chrono::steady_clock::now () >= deadline)
                return false;
            std::this_thread::yield ();
        }
        return true;
    }
};

} // namespace

TEST (SunStudyAdvanceWorker, SubmissionDoesNotCalculateOrWaitOnTheCaller)
{
    PausedStudy study;
    ASSERT_TRUE (study.worker.Submit (study.request, study.error)) << study.error;
    ASSERT_TRUE (study.Started ());
    EXPECT_NE (study.gate->thread, std::this_thread::get_id ());
    EXPECT_TRUE (study.worker.Busy ());
    EXPECT_FALSE (study.worker.Submit (study.request, study.error));
    StudyProgress progress;
    EXPECT_TRUE (SunStudyStore::Get ().Progress (study.request.studyId, progress, study.error));
    EXPECT_EQ (progress.resolvedSteps, 0u);
    AdvanceCompletion completion;
    EXPECT_FALSE (study.worker.Poll (completion));
    study.Release ();
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_TRUE (completion.succeeded) << completion.error;
    EXPECT_TRUE (completion.progress.converged);
    EXPECT_EQ (completion.advanced, 3u);
    EXPECT_EQ (completion.threadId, study.gate->thread);
    EXPECT_GE (completion.wallMilliseconds, 0.0);
    EXPECT_EQ (completion.request.sessionGeneration, 7u);
    EXPECT_EQ (completion.request.runGeneration, 11u);
    EXPECT_EQ (completion.request.recordRevision, SunStudyStore::Get ().Revision (study.request.studyId));
    EXPECT_FALSE (study.worker.Poll (completion));
    EXPECT_FALSE (study.worker.Busy ());
    std::vector<double> hours, positions;
    ASSERT_TRUE (SunStudyStore::Get ().SunHours (study.request.studyId, hours, positions, study.error));
    EXPECT_EQ (hours, std::vector<double> { 3.0 });
}

TEST (SunStudyAdvanceWorker, CancelDoesNotJoinAndStopsBeforeTheNextTimestep)
{
    PausedStudy study;
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    ASSERT_TRUE (study.Started ());
    study.worker.Cancel (); // would deadlock here if cancellation joined
    EXPECT_TRUE (study.worker.Busy ());
    EXPECT_FALSE (study.worker.Submit (study.request, study.error));
    study.Release ();
    ASSERT_TRUE (study.WaitIdle ());
    EXPECT_EQ (study.gate->calls.load (), 1u);
    AdvanceCompletion completion;
    EXPECT_FALSE (study.worker.Poll (completion));
    ++study.request.sessionGeneration;
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_EQ (completion.request.sessionGeneration, 8u);
    EXPECT_EQ (completion.advanced, 2u);
    EXPECT_TRUE (completion.progress.converged);
}

TEST (SunStudyAdvanceWorker, EraseDuringCalculationStopsTheRunWithoutResurrectingIt)
{
    PausedStudy study;
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    ASSERT_TRUE (study.Started ());
    ASSERT_TRUE (SunStudyStore::Get ().Erase (study.request.studyId));
    study.Release ();
    AdvanceCompletion completion;
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_FALSE (completion.succeeded);
    EXPECT_EQ (study.gate->calls.load (), 1u);
    EXPECT_TRUE (SunStudyStore::Get ().Ids ().empty ());
}

TEST (SunStudyAdvanceWorker, OldWorkCannotAdvanceOrPublishIntoAReplacementWithTheSameId)
{
    PausedStudy study;
    const uint64_t oldRevision = SunStudyStore::Get ().Revision (study.request.studyId);
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    ASSERT_TRUE (study.Started ());
    ASSERT_TRUE (SunStudyStore::Get ().Erase (study.request.studyId));
    auto replacement = Record (study.traversal);
    replacement->id = study.request.studyId;
    ASSERT_EQ (SunStudyStore::Get ().Insert (std::move (replacement)), study.request.studyId);
    study.Release ();
    AdvanceCompletion completion;
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_FALSE (completion.succeeded);
    EXPECT_EQ (completion.error, "sun study record changed");
    StudyProgress progress;
    ASSERT_TRUE (SunStudyStore::Get ().Progress (study.request.studyId, progress, study.error));
    EXPECT_EQ (progress.resolvedSteps, 0u);
    size_t advanced = 99;
    EXPECT_FALSE (SunStudyStore::Get ().Advance (study.request.studyId, 3, 1, 0.001, 0.0, advanced, study.error,
                                                 nullptr, oldRevision));
    EXPECT_EQ (advanced, 0u);
    EXPECT_FALSE (SunStudyStore::Get ().Erase (study.request.studyId, oldRevision));
    EXPECT_EQ (SunStudyStore::Get ().Count (), 1u);
    study.request.recordRevision = oldRevision;
    EXPECT_FALSE (study.worker.Submit (study.request, study.error));
}

TEST (SunStudyAdvanceWorker, CancellationDiscardsAnUnpolledCompletion)
{
    PausedStudy study;
    study.Release ();
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    StudyProgress progress;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    do {
        ASSERT_LT (std::chrono::steady_clock::now (), deadline);
        ASSERT_TRUE (SunStudyStore::Get ().Progress (study.request.studyId, progress, study.error));
        std::this_thread::yield ();
    } while (!progress.converged);
    study.worker.Cancel ();
    ASSERT_TRUE (study.WaitIdle ());
    AdvanceCompletion completion;
    EXPECT_FALSE (study.worker.Poll (completion));
}

TEST (SunStudyAdvanceWorker, ShutdownIsIdempotentAndNeverFallsBackToInlineWork)
{
    PausedStudy study;
    study.worker.Shutdown ();
    study.worker.Shutdown ();
    EXPECT_FALSE (study.worker.Submit (study.request, study.error));
    EXPECT_EQ (study.gate->calls.load (), 0u);
    EXPECT_FALSE (study.worker.Busy ());
}

TEST (SunStudyAdvanceWorker, SoftTimeBudgetStopsBetweenCompleteStepsAndCanResume)
{
    PausedStudy study;
    study.request.maxMilliseconds = 0.000000001;
    study.Release ();
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    AdvanceCompletion completion;
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_TRUE (completion.succeeded);
    EXPECT_EQ (completion.advanced, 1u);
    EXPECT_EQ (completion.progress.resolvedSteps, 1u);
    EXPECT_FALSE (completion.progress.converged);
    study.request.maxMilliseconds = 0.0;
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_TRUE (completion.succeeded);
    EXPECT_EQ (completion.advanced, 2u);
    EXPECT_TRUE (completion.progress.converged);
}

TEST (SunStudyAdvanceWorker, AFailedSliceDoesNotKillTheWorkerOrPoisonTheSession)
{
    PausedStudy study;
    study.gate->throwNext.store (true);
    study.Release ();
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    AdvanceCompletion completion;
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_FALSE (completion.succeeded);
    EXPECT_NE (completion.error.find ("test traversal failure"), std::string::npos);
    EXPECT_NE (completion.threadId, std::this_thread::get_id ());
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_TRUE (completion.succeeded) << completion.error;
    EXPECT_TRUE (completion.progress.converged);
    EXPECT_EQ (completion.advanced, 3u);
}

TEST (SunStudyAdvanceWorker, ClearDuringCalculationStopsAtTheCurrentStep)
{
    PausedStudy study;
    ASSERT_TRUE (study.worker.Submit (study.request, study.error));
    ASSERT_TRUE (study.Started ());
    SunStudyStore::Get ().Clear ();
    study.Release ();
    AdvanceCompletion completion;
    ASSERT_TRUE (study.WaitResult (completion));
    EXPECT_FALSE (completion.succeeded);
    EXPECT_EQ (study.gate->calls.load (), 1u);
    EXPECT_EQ (SunStudyStore::Get ().Count (), 0u);
}

TEST (SunStudySession, CooperativeCancellationCommitsOnlyCompleteStepsAndCanResume)
{
    PausedStudy study;
    study.Release ();
    auto record = Record (study.traversal);
    size_t checks = 0;
    EXPECT_EQ (record->session.Advance (*study.traversal, 3, 0.001, 0.0, 1, [&checks] () { return checks++ > 0; }), 1u);
    EXPECT_EQ (record->session.Progress ().resolvedSteps, 1u);
    EXPECT_FALSE (record->session.Progress ().converged);
    EXPECT_EQ (record->session.Advance (*study.traversal, 3, 0.001, 0.0, 1), 2u);
    EXPECT_TRUE (record->session.Progress ().converged);
    EXPECT_EQ (record->session.SunHours (), std::vector<double> { 3.0 });
}
