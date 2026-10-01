#include "SunStudy/SunStudyTaskWorker.hpp"
#include "SunStudy/SunStudyRefreshSchedule.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>

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
    EXPECT_FALSE (schedule.Ready (600, true));
    EXPECT_TRUE (schedule.Pending ());
    EXPECT_TRUE (schedule.Ready (601, false));
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
