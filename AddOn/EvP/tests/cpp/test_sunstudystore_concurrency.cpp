#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace evp::sunstudy;

namespace {

struct TraversalGate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future ().share ();
};

class PausedTraversal final : public ITraversal {
  public:
    explicit PausedTraversal (std::shared_ptr<TraversalGate> gate) : gate_ (std::move (gate))
    {
    }
    void OccludeDirectional (const double* origins, size_t count, const double*, double, double, uint8_t* out,
                             size_t) const override
    {
        gate_->entered.set_value ();
        gate_->released.wait ();
        // Access after release proves the record's sample buffers survived Erase.
        for (size_t index = 0; index < count; ++index)
            out[index] = origins[index * 3] < 0.0 ? 1u : 0u;
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
    std::shared_ptr<TraversalGate> gate_;
};

std::unique_ptr<StudyRecord> Record (std::shared_ptr<const ITraversal> traversal)
{
    auto record = std::make_unique<StudyRecord> ();
    record->positions = { 1.0, 0.0, 0.0 };
    record->normals = { 0.0, 0.0, 1.0 };
    record->traversal = std::move (traversal);
    SunStep step;
    step.time = { 12, 0 };
    step.altitudeDegrees = 90.0;
    step.direction[2] = 1.0;
    record->series = SunSeries::FromSteps ({ step }, 60);
    record->session.Sync ({ 31, record->series.Version (), 1 }, record->series, record->Samples ());
    return record;
}

// Always release/join, including when an ASSERT exits a test early.
struct RunningSlice {
    std::shared_ptr<TraversalGate> gate = std::make_shared<TraversalGate> ();
    std::string id;
    size_t advanced = 0;
    std::string error;
    bool succeeded = false;
    std::thread worker;

    RunningSlice ()
    {
        SunStudyStore::Get ().Clear ();
        id = SunStudyStore::Get ().Insert (Record (std::make_shared<PausedTraversal> (gate)));
        auto entered = gate->entered.get_future ();
        worker = std::thread (
            [this] () { succeeded = SunStudyStore::Get ().Advance (id, 1, 1, 0.001, 0.0, advanced, error); });
        EXPECT_EQ (entered.wait_for (std::chrono::seconds (5)), std::future_status::ready);
    }
    ~RunningSlice ()
    {
        Finish ();
        SunStudyStore::Get ().Clear ();
    }
    void Finish ()
    {
        if (worker.joinable ()) {
            gate->release.set_value ();
            worker.join ();
        }
    }
};

class ThrowOnceTraversal final : public ITraversal {
  public:
    void OccludeDirectional (const double*, size_t count, const double*, double, double, uint8_t* out,
                             size_t) const override
    {
        if (throwNext_.exchange (false))
            throw std::runtime_error ("test traversal failure");
        std::fill (out, out + count, uint8_t { 0 });
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
    mutable std::atomic<bool> throwNext_ { true };
};

} // namespace

TEST (SunStudyStoreConcurrency, ProgressReportsTheLastCompletedSliceWithoutWaiting)
{
    RunningSlice slice;
    StudyProgress progress;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Progress (slice.id, progress, error));
    EXPECT_EQ (progress.resolvedSteps, 0u);
    EXPECT_FALSE (progress.converged);
    slice.Finish ();
    ASSERT_TRUE (SunStudyStore::Get ().Progress (slice.id, progress, error));
    EXPECT_EQ (progress.resolvedSteps, 1u);
    EXPECT_TRUE (progress.converged);
}

TEST (SunStudyStoreConcurrency, AllMutableSessionReadsRefuseAnInFlightSlice)
{
    RunningSlice slice;
    auto& store = SunStudyStore::Get ();
    std::vector<double> hours, positions, normals;
    std::vector<float> image;
    std::vector<AtlasTile> tiles;
    std::vector<FaceLayout> layouts;
    StepMaskAtlas masks;
    std::vector<uint16_t> minutes;
    uint32_t width = 0, height = 0, noon = 0;
    double spacing = 0.0, daylight = 0.0;
    bool converged = false;
    uint64_t generation = 0;
    std::string error;
    EXPECT_FALSE (store.SunHours (slice.id, hours, positions, error));
    EXPECT_EQ (error, "computing");
    EXPECT_FALSE (store.Results (slice.id, hours, positions, normals, nullptr, error));
    EXPECT_FALSE (store.AtlasImage (slice.id, width, height, image, error));
    EXPECT_FALSE (store.PatchAtlasImage (slice.id, width, height, image, error));
    EXPECT_FALSE (store.DisplayData (slice.id, tiles, layouts, width, height, spacing, image, daylight, converged,
                                     generation, error));
    EXPECT_FALSE (store.StepMasks (slice.id, masks, minutes, noon, error));
    SunStudyReading reading;
    uint8_t role = 0;
    const double point[3] = { 0.0, 0.0, 0.0 };
    EXPECT_FALSE (store.ReadAt (slice.id, 31, 0, 0, point, reading, role, daylight, error));
    EXPECT_EQ (error, "computing");
    StudyRecord metadata;
    EXPECT_TRUE (store.Describe (slice.id, metadata, error));
    size_t advanced = 0;
    EXPECT_FALSE (store.Advance (slice.id, 1, 1, 0.001, 0.0, advanced, error));
    EXPECT_EQ (advanced, 0u);
}

TEST (SunStudyStoreConcurrency, EraseDuringAdvanceDoesNotDestroyBorrowedSampleBuffers)
{
    RunningSlice slice;
    EXPECT_TRUE (SunStudyStore::Get ().Erase (slice.id));
    EXPECT_EQ (SunStudyStore::Get ().Count (), 0u);
    slice.Finish ();
    EXPECT_TRUE (slice.succeeded) << slice.error;
    // The owned buffers survived the blocked traversal, but cancellation now
    // rolls back its uncommitted timestep instead of resolving stale bits.
    EXPECT_EQ (slice.advanced, 0u);
    StudyProgress progress;
    std::string error;
    EXPECT_FALSE (SunStudyStore::Get ().Progress (slice.id, progress, error));
}

TEST (SunStudyStoreConcurrency, ClearDuringAdvanceIsSafeAndDoesNotResurrectTheStudy)
{
    RunningSlice slice;
    SunStudyStore::Get ().Clear ();
    slice.Finish ();
    EXPECT_TRUE (slice.succeeded) << slice.error;
    EXPECT_TRUE (SunStudyStore::Get ().Ids ().empty ());
}

TEST (SunStudyStoreConcurrency, ALateCompletionCannotPublishIntoAReplacementWithTheSameId)
{
    RunningSlice slice;
    auto& store = SunStudyStore::Get ();
    ASSERT_TRUE (store.Erase (slice.id));
    auto replacement = Record (nullptr);
    replacement->id = slice.id;
    ASSERT_EQ (store.Insert (std::move (replacement)), slice.id);
    slice.Finish ();
    StudyProgress progress;
    std::string error;
    ASSERT_TRUE (store.Progress (slice.id, progress, error));
    EXPECT_EQ (progress.resolvedSteps, 0u);
    EXPECT_FALSE (progress.converged);
    StudyRecord metadata;
    ASSERT_TRUE (store.Describe (slice.id, metadata, error));
    EXPECT_DOUBLE_EQ (metadata.analysisMilliseconds, 0.0);
}

TEST (SunStudyStoreConcurrency, DuplicateInsertCannotReplaceAnAdvancingRecord)
{
    RunningSlice slice;
    auto duplicate = Record (nullptr);
    duplicate->id = slice.id;
    EXPECT_TRUE (SunStudyStore::Get ().Insert (std::move (duplicate)).empty ());
    EXPECT_EQ (SunStudyStore::Get ().Count (), 1u);
}

TEST (SunStudyStoreConcurrency, ATraversalExceptionReleasesBothDispatchGuardsForRetry)
{
    auto& store = SunStudyStore::Get ();
    store.Clear ();
    const std::string id = store.Insert (Record (std::make_shared<ThrowOnceTraversal> ()));
    size_t advanced = 0;
    std::string error;
    EXPECT_FALSE (store.Advance (id, 1, 1, 0.001, 0.0, advanced, error));
    EXPECT_NE (error.find ("test traversal failure"), std::string::npos);
    ASSERT_TRUE (store.Advance (id, 1, 1, 0.001, 0.0, advanced, error)) << error;
    EXPECT_EQ (advanced, 1u);
    StudyProgress progress;
    ASSERT_TRUE (store.Progress (id, progress, error));
    EXPECT_TRUE (progress.converged);
    store.Clear ();
}

TEST (SunStudyStoreConcurrency, ManualAndAutomaticRecordsShareOneCalculationLaneWithoutBlockingProgress)
{
    RunningSlice first;
    auto& store = SunStudyStore::Get ();
    auto gate = std::make_shared<TraversalGate> ();
    gate->release.set_value ();
    auto entered = gate->entered.get_future ();
    const auto id = store.Insert (Record (std::make_shared<PausedTraversal> (gate)));
    size_t advanced = 0;
    std::string error;
    auto second =
        std::async (std::launch::async, [&] { return store.Advance (id, 1, 1, 0.001, 0.0, advanced, error); });
    EXPECT_EQ (entered.wait_for (std::chrono::milliseconds (50)), std::future_status::timeout);
    StudyProgress progress;
    EXPECT_TRUE (store.Progress (id, progress, error));
    EXPECT_EQ (progress.resolvedSteps, 0u);
    first.Finish ();
    EXPECT_TRUE (second.get ());
    EXPECT_EQ (advanced, 1u);
    StudyRecord metadata;
    EXPECT_TRUE (store.Describe (id, metadata, error));
    EXPECT_GT (metadata.admissionMilliseconds, 0.0);
}

TEST (SunStudyStoreConcurrency, CancelledQueuedRecordNeverTracesOrWaitsForTheActiveStudy)
{
    RunningSlice first;
    auto& store = SunStudyStore::Get ();
    auto gate = std::make_shared<TraversalGate> ();
    gate->release.set_value ();
    auto entered = gate->entered.get_future ();
    const auto id = store.Insert (Record (std::make_shared<PausedTraversal> (gate)));
    size_t advanced = 0;
    std::string error;
    std::atomic<bool> cancelled { false };
    auto queued = std::async (std::launch::async,
                              [&] { return store.Advance (id, 1, 1, 0.001, 0.0, advanced, error, &cancelled); });
    EXPECT_EQ (entered.wait_for (std::chrono::milliseconds (30)), std::future_status::timeout);
    cancelled.store (true);
    const auto finished = queued.wait_for (std::chrono::seconds (2));
    first.Finish (); // always release before a future destructor, even on failure
    EXPECT_EQ (finished, std::future_status::ready);
    EXPECT_TRUE (queued.get ());
    EXPECT_EQ (advanced, 0u);
    EXPECT_EQ (entered.wait_for (std::chrono::milliseconds (0)), std::future_status::timeout);
    StudyProgress progress;
    EXPECT_TRUE (store.Progress (id, progress, error));
    EXPECT_EQ (progress.resolvedSteps, 0u);
}
