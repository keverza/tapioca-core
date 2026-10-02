#include "SunStudy/SunStudyTaskWorker.hpp"
#include "SunStudy/SunStudyRefreshSchedule.hpp"
#include "SunStudy/SunStudyFollower.hpp"
#include "SunStudy/SunStudyPreviewPlan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <limits>
#include <vector>

using namespace evp::sunstudy;

namespace {

struct PausedTask {
    std::promise<void> entered;
    std::future<void> entry = entered.get_future ();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future ().share ();
    std::atomic<size_t> executions { 0 };
    std::atomic<size_t> discards { 0 };
    std::thread::id executionThread;
    std::thread::id discardThread;
    SunStudyTaskWorker worker;
    StudyTaskRequest request;
    std::string error;
    bool opened = false;

    PausedTask ()
    {
        request.sessionGeneration = 7;
        request.runGeneration = 13;
        request.execute = [this] (const std::atomic<bool>&) {
            executionThread = std::this_thread::get_id ();
            executions.fetch_add (1);
            entered.set_value ();
            released.wait ();
        };
        request.discard = [this] () {
            discardThread = std::this_thread::get_id ();
            discards.fetch_add (1);
        };
    }
    ~PausedTask ()
    {
        Release ();
        worker.Shutdown ();
    }
    void Release ()
    {
        if (!opened) {
            opened = true;
            release.set_value ();
        }
    }
    bool Started ()
    {
        return entry.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
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
    bool WaitResult (StudyTaskCompletion& completion)
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

TEST (SunStudyTaskWorker, SubmitReturnsBeforeWorkCompletesAndOnlyOneTaskCanRun)
{
    PausedTask task;
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.Started ());
    EXPECT_NE (task.executionThread, std::this_thread::get_id ());
    EXPECT_FALSE (task.worker.Submit (task.request, task.error));
    StudyTaskCompletion completion;
    EXPECT_FALSE (task.worker.Poll (completion));
    task.Release ();
    ASSERT_TRUE (task.WaitResult (completion));
    EXPECT_EQ (completion.sessionGeneration, 7u);
    EXPECT_EQ (completion.runGeneration, 13u);
    EXPECT_EQ (completion.threadId, task.executionThread);
    EXPECT_TRUE (completion.error.empty ());
    EXPECT_FALSE (task.worker.Busy ());
    EXPECT_EQ (task.discards.load (), 0u);
}

TEST (SunStudyTaskWorker, CancelDoesNotJoinAndDiscardsLargeResultsOnTheWorker)
{
    PausedTask task;
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.Started ());
    task.worker.Cancel ();
    task.worker.Cancel ();
    EXPECT_TRUE (task.worker.Busy ());
    task.Release ();
    ASSERT_TRUE (task.WaitIdle ());
    EXPECT_EQ (task.discards.load (), 1u);
    EXPECT_EQ (task.discardThread, task.executionThread);
    StudyTaskCompletion completion;
    EXPECT_FALSE (task.worker.Poll (completion));
    task.request.execute = [] (const std::atomic<bool>&) {};
    ++task.request.sessionGeneration;
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.WaitResult (completion));
    EXPECT_EQ (completion.sessionGeneration, 8u);
    EXPECT_EQ (task.discards.load (), 1u);
}

TEST (SunStudyTaskWorker, CancelAnUnpolledCompletionStillCleansUpOffThread)
{
    PausedTask task;
    task.Release ();
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.Started ());
    task.worker.Cancel ();
    ASSERT_TRUE (task.WaitIdle ());
    EXPECT_EQ (task.discards.load (), 1u);
    EXPECT_NE (task.discardThread, std::this_thread::get_id ());
    StudyTaskCompletion completion;
    EXPECT_FALSE (task.worker.Poll (completion));
}

TEST (SunStudyTaskWorker, FailureCleansUpOnceAndDoesNotPoisonTheMailbox)
{
    PausedTask task;
    task.request.execute = [] (const std::atomic<bool>&) { throw std::runtime_error ("test preparation failure"); };
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    StudyTaskCompletion completion;
    ASSERT_TRUE (task.WaitResult (completion));
    EXPECT_EQ (completion.error, "test preparation failure");
    EXPECT_EQ (task.discards.load (), 1u);
    task.request.execute = [] (const std::atomic<bool>&) {};
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.WaitResult (completion));
    EXPECT_TRUE (completion.error.empty ());
}

TEST (SunStudyTaskWorker, ShutdownIsIdempotentAndRefusesNewTasks)
{
    PausedTask task;
    task.worker.Shutdown ();
    task.worker.Shutdown ();
    EXPECT_FALSE (task.worker.Submit (task.request, task.error));
    EXPECT_FALSE (task.worker.Busy ());
    EXPECT_EQ (task.executions.load (), 0u);
}

TEST (SunStudyTaskWorker, PreparationNotificationCanPollOutsideTheMailboxLock)
{
    PausedTask task;
    std::promise<StudyTaskCompletion> notified;
    auto result = notified.get_future ();
    task.request.onReady = [&task, &notified] () {
        StudyTaskCompletion completion;
        if (task.worker.Poll (completion))
            notified.set_value (std::move (completion));
    };
    task.Release ();
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_EQ (result.wait_for (std::chrono::seconds (5)), std::future_status::ready);
    EXPECT_TRUE (result.get ().error.empty ());
    EXPECT_FALSE (task.worker.Busy ());
    task.worker.Shutdown ();
}

TEST (SunStudyTaskWorker, CancelledPreparationDoesNotNotify)
{
    PausedTask task;
    std::atomic<size_t> wakes { 0 };
    task.request.onReady = [&wakes] () { ++wakes; };
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.Started ());
    task.worker.Cancel ();
    task.Release ();
    ASSERT_TRUE (task.WaitIdle ());
    EXPECT_EQ (wakes.load (), 0u);
}

TEST (SunStudyPreviewPlan, SmallStudiesKeepOnlyTheRequestedGrid)
{
    const auto plan = MakeSunStudyPreviewPlan (65535, 0.25);
    EXPECT_FALSE (plan.enabled);
    EXPECT_EQ (plan.previewSpacing, 0.25);
    EXPECT_EQ (plan.requestedSpacing, 0.25);
}

TEST (SunStudyPreviewPlan, LargeStudiesPreviewWithoutChangingTheFinalResolution)
{
    const auto plan = MakeSunStudyPreviewPlan (300000, 0.25);
    EXPECT_TRUE (plan.enabled);
    EXPECT_EQ (plan.previewSpacing, 1.0);
    EXPECT_EQ (plan.requestedSpacing, 0.25);
}

TEST (SunStudyPreviewPlan, InvalidOrOverflowingSpacingCannotStartACoarsePreview)
{
    for (double spacing : { 0.0, -1.0, std::numeric_limits<double>::infinity (),
                            std::numeric_limits<double>::quiet_NaN (), std::numeric_limits<double>::max () })
        EXPECT_FALSE (MakeSunStudyPreviewPlan (300000, spacing).enabled);
}

TEST (SunStudyTaskWorker, ShutdownDrainsAnOutstandingTaskAndItsDiscard)
{
    PausedTask task;
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.Started ());
    task.Release ();
    task.worker.Shutdown ();
    EXPECT_EQ (task.discards.load (), 1u);
    EXPECT_FALSE (task.worker.Busy ());
}

TEST (SunStudyTaskWorker, ThrowingCleanupCannotKillTheWorker)
{
    PausedTask task;
    task.request.discard = [] () { throw std::runtime_error ("test discard failure"); };
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.Started ());
    task.worker.Cancel ();
    task.Release ();
    ASSERT_TRUE (task.WaitIdle ());
    task.request.execute = [] (const std::atomic<bool>&) {};
    StudyTaskCompletion completion;
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.WaitResult (completion));
    EXPECT_TRUE (completion.error.empty ());
}

TEST (SunStudyRefreshSchedule, NavigationWithoutAnEditNeverTriggersCapture)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (41);
    EXPECT_FALSE (schedule.Observe (41, 0));
    EXPECT_FALSE (schedule.Ready (10000, true));
    EXPECT_FALSE (schedule.Ready (10000, false));
}

TEST (SunStudyRefreshSchedule, RepeatedPollingDoesNotResetTheQuietPeriod)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (41);
    EXPECT_TRUE (schedule.Observe (42, 100));
    EXPECT_FALSE (schedule.Observe (42, 200));
    EXPECT_FALSE (schedule.Ready (399, false));
    EXPECT_TRUE (schedule.Ready (400, false));
    schedule.Complete ();
    EXPECT_FALSE (schedule.Pending ());
    EXPECT_FALSE (schedule.Ready (1000, false));
}

TEST (SunStudyRefreshSchedule, EditBurstsCoalesceButNavigationDefersCapture)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (41);
    EXPECT_TRUE (schedule.Observe (42, 100));
    EXPECT_TRUE (schedule.Observe (43, 300));
    EXPECT_FALSE (schedule.Ready (400, false));
    EXPECT_FALSE (schedule.Ready (1049, false));
    EXPECT_FALSE (schedule.Ready (1050, true));
    EXPECT_TRUE (schedule.Pending ());
    EXPECT_TRUE (schedule.Ready (1050, false));
    EXPECT_EQ (schedule.PendingSignals (), 2u);
    EXPECT_EQ (schedule.PendingObservations (), 2u);
}

TEST (SunStudyRefreshSchedule, FailedCaptureRemainsPendingAndNewDocumentsResetIt)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (41);
    schedule.Observe (42, 100);
    EXPECT_TRUE (schedule.Ready (400, false));
    EXPECT_TRUE (schedule.Ready (600, false));
    schedule.Reset (0);
    EXPECT_FALSE (schedule.Pending ());
    EXPECT_FALSE (schedule.Observe (0, 800));
}

TEST (SunStudyRefreshSchedule, HundredsOfEditsKeepOneLatestTargetUntilTheLastQuietWindow)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (0);
    for (uint32_t edit = 1; edit <= 500; ++edit) {
        const int64_t now = edit * 750;
        ASSERT_TRUE (schedule.Observe (edit, now));
        EXPECT_TRUE (schedule.Pending ());
        if (edit > 1)
            EXPECT_FALSE (schedule.Ready (now + 749, false));
    }
    EXPECT_EQ (schedule.PendingSignals (), 500u);
    EXPECT_EQ (schedule.PendingObservations (), 500u);
    EXPECT_EQ (schedule.BatchStartedMs (), 750);
    EXPECT_EQ (schedule.LastEditMs (), 375000);
    EXPECT_EQ (schedule.QuietMilliseconds (), 1500);
    EXPECT_FALSE (schedule.Ready (376499, false));
    EXPECT_TRUE (schedule.Ready (376500, false));
    schedule.Complete ();
    EXPECT_FALSE (schedule.Pending ());
    EXPECT_EQ (schedule.PendingSignals (), 0u);
    EXPECT_EQ (schedule.PendingObservations (), 0u);
    EXPECT_FALSE (schedule.Observe (500, 400000));
}

TEST (SunStudyRefreshSchedule, NearbyEditsAfterCaptureStillBelongToTheBurst)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (0);
    ASSERT_TRUE (schedule.Observe (1, 100));
    ASSERT_TRUE (schedule.Ready (400, false));
    schedule.Complete ();
    ASSERT_TRUE (schedule.Observe (2, 850));
    EXPECT_EQ (schedule.QuietMilliseconds (), 750);
    EXPECT_EQ (schedule.PendingSignals (), 1u);
    EXPECT_EQ (schedule.BatchStartedMs (), 850);
    ASSERT_TRUE (schedule.Observe (3, 1600));
    EXPECT_EQ (schedule.QuietMilliseconds (), 1500);
    EXPECT_FALSE (schedule.Ready (3099, false));
    EXPECT_TRUE (schedule.Ready (3100, false));
}

TEST (SunStudyRefreshSchedule, AnIsolatedEditRecoversTheShortDelayAndPollingCannotExtendIt)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (0);
    ASSERT_TRUE (schedule.Observe (3, 100));
    EXPECT_EQ (schedule.QuietMilliseconds (), 1500);
    schedule.Complete ();
    ASSERT_TRUE (schedule.Observe (4, 2101));
    EXPECT_EQ (schedule.QuietMilliseconds (), 300);
    for (int64_t now = 2150; now <= 2400; now += 50)
        EXPECT_FALSE (schedule.Observe (4, now));
    EXPECT_EQ (schedule.MillisecondsUntilReady (2400), 1);
    EXPECT_TRUE (schedule.Ready (2401, false));
}

TEST (SunStudyRefreshSchedule, CounterWrapAndSessionResetDoNotLoseOrReplayEdits)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (std::numeric_limits<uint32_t>::max () - 1);
    ASSERT_TRUE (schedule.Observe (1, 100));
    EXPECT_EQ (schedule.PendingSignals (), 3u);
    EXPECT_EQ (schedule.PendingObservations (), 1u);
    EXPECT_EQ (schedule.QuietMilliseconds (), 1500);
    EXPECT_FALSE (schedule.Ready (1600, true));
    EXPECT_TRUE (schedule.Ready (1600, false));
    schedule.Reset (0);
    EXPECT_EQ (schedule.QuietMilliseconds (), 300);
    EXPECT_EQ (schedule.MillisecondsUntilReady (10000), -1);
    EXPECT_EQ (schedule.PendingSignals (), 0u);
    ASSERT_TRUE (schedule.Observe (1, 10001));
    EXPECT_TRUE (schedule.Ready (10301, false));
}

TEST (SunStudyRefreshSchedule, WatchCadenceBurstCapturesOnlyTheFirstAndLatestModel)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (0);
    std::vector<uint32_t> captures;
    for (int64_t now = 0; now <= 77000; now += 50) {
        const uint32_t edits = static_cast<uint32_t> (std::min<int64_t> (now / 750, 100));
        schedule.Observe (edits, now);
        if (schedule.Ready (now, false)) {
            captures.push_back (edits);
            schedule.Complete ();
        }
    }
    ASSERT_EQ (captures.size (), 2u);
    EXPECT_EQ (captures.front (), 1u);
    EXPECT_EQ (captures.back (), 100u);
    EXPECT_FALSE (schedule.Pending ());
}

TEST (SunStudyRefreshSchedule, CapturedGeometryDoesNotWaitThroughASecondFollowerDebounce)
{
    SunStudyRefreshSchedule schedule;
    schedule.Reset (0);
    SunStudyFollower follower;
    const SunStudyDependencySignature original { 1, 2, 3 };
    const SunStudyDependencySignature edited { 2, 2, 3 };
    follower.Adopt ("original", original, 0);
    schedule.Observe (1, 100);
    ASSERT_TRUE (schedule.Ready (400, false));
    follower.Observe (edited, schedule.LastEditMs ());
    schedule.Complete ();
    EXPECT_TRUE (follower.ShouldStart (400));
    EXPECT_EQ (follower.MillisecondsUntilStart (400), 0);
    const auto generation = follower.NoteStarted (edited, 400);
    EXPECT_TRUE (follower.CanPublishResult (generation, edited));
    ASSERT_TRUE (follower.NoteCompleted (generation, "edited", edited, 500));
    // An unrelated sun-input change has not passed the geometry quiet window.
    follower.Observe ({ 2, 4, 3 }, 600);
    EXPECT_FALSE (follower.ShouldStart (899));
    EXPECT_TRUE (follower.ShouldStart (900));
}

TEST (SunStudyRefreshSchedule, CancelledPreparationCannotBuildACaptureBacklog)
{
    PausedTask task;
    ASSERT_TRUE (task.worker.Submit (task.request, task.error));
    ASSERT_TRUE (task.Started ());
    SunStudyRefreshSchedule schedule;
    schedule.Reset (0);
    for (uint32_t edit = 1; edit <= 50; ++edit) {
        schedule.Observe (edit, edit * 750);
        task.worker.Cancel ();
        EXPECT_FALSE (schedule.Ready (edit * 750 + 1500, false) && !task.worker.Busy ());
        EXPECT_FALSE (task.worker.Submit (task.request, task.error));
    }
    task.Release ();
    ASSERT_TRUE (task.WaitIdle ());
    EXPECT_TRUE (schedule.Ready (50 * 750 + 1500, false));
    EXPECT_EQ (schedule.PendingSignals (), 50u);
    EXPECT_EQ (task.executions.load (), 1u);
    EXPECT_EQ (task.discards.load (), 1u);
    schedule.Complete ();
    EXPECT_FALSE (schedule.Pending ());
}
