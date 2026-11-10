// How a stoppable caller waits for its main-thread job (evp::gatewait::Wait, #114).
//
// The live failure: the main thread started an extraction pass and, in the same
// turn, restarted it -- Stop() joined the worker while the worker waited in
// Invoke for that same main thread. Here the "main thread" is the test body: it
// never serves the queue, it only sets the stop flag and joins.

#include "Python/GateWait.hpp"
#include "Python/OwnedGateCall.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <future>

TEST (OwnedGateCall, TimeoutAfterDispatchRetainsRequestAndResult)
{
    std::promise<void> entered, resume;
    auto released = resume.get_future ().share ();
    auto started = entered.get_future ();
    std::thread dispatch;
    auto request = std::make_shared<int> (42);
    std::weak_ptr<int> lifetime = request;
    auto result = tapioca::InvokeOwnedGateCall<int> (
        [&] (auto job) {
            dispatch = std::thread (std::move (job));
            started.wait ();
            return false; // deterministic timeout AFTER dispatch, before SDK completion
        },
        [request, released, &entered] (int& output) {
            entered.set_value ();
            released.wait ();
            output = *request;
        });
    request.reset ();
    EXPECT_EQ (result, nullptr);
    EXPECT_FALSE (lifetime.expired ());
    resume.set_value ();
    dispatch.join ();
    EXPECT_TRUE (lifetime.expired ());
}

TEST (OwnedGateCall, RevokedOrShutdownJobCannotRunAfterCallerLeaves)
{
    std::function<void ()> queued;
    int calls = 0;
    auto result = tapioca::InvokeOwnedGateCall<int> (
        [&] (auto job) {
            queued = std::move (job);
            return false;
        },
        [&calls] (int&) { ++calls; });
    queued (); // a dispatch that lost the revoke race is still abandoned
    EXPECT_EQ (result, nullptr);
    EXPECT_EQ (calls, 0);
    queued = {};
    result = tapioca::InvokeOwnedGateCall<int> ([] (auto) { return false; }, [&calls] (int&) { ++calls; });
    EXPECT_EQ (result, nullptr);
    EXPECT_EQ (calls, 0);
}

TEST (OwnedGateCall, CompletedResultIsPublished)
{
    auto result = tapioca::InvokeOwnedGateCall<int> (
        [] (auto job) {
            job ();
            return true;
        },
        [] (int& value) { value = 42; });
    ASSERT_NE (result, nullptr);
    EXPECT_EQ (*result, 42);
}

using evp::gatewait::Outcome;
using evp::gatewait::Wait;

namespace {

using Clock = std::chrono::steady_clock;

int64_t MsSince (Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (Clock::now () - start).count ();
}

// One posted job: its completion, and whether the queue still holds it.
struct Job {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool queued = true;
    bool ran = false;

    bool WaitFor (int ms)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return cv.wait_for (lock, std::chrono::milliseconds (ms), [this] { return done; });
    }
    bool Revoke ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (!queued)
            return false;
        queued = false;
        return true;
    }
    // The main thread dispatches it: out of the queue, then run, then done.
    void Dispatch ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        queued = false;
    }
    void Finish ()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            ran = true;
            done = true;
        }
        cv.notify_all ();
    }
};

Outcome WaitOn (Job& job, int timeoutMs, const std::atomic<bool>& stop)
{
    return Wait (
        timeoutMs, [&job] (int ms) { return job.WaitFor (ms); }, [&stop] { return stop.load (); },
        [&job] { return job.Revoke (); });
}

} // namespace

// The 2026-11-01 menu open: 20 s of Archicad frozen, nothing generating.
TEST (GateWait, AJoiningMainThreadGetsItsThreadBackAtOnce)
{
    Job job;
    std::atomic<bool> stop { false };
    Outcome outcome = Outcome::Completed;
    std::thread worker ([&] { outcome = WaitOn (job, 20000, stop); });

    std::this_thread::sleep_for (std::chrono::milliseconds (30)); // the worker is waiting now
    const Clock::time_point joinStart = Clock::now ();
    stop.store (true); // ExtractionWorker::Stop: flag, then join, from the main thread
    worker.join ();

    EXPECT_LT (MsSince (joinStart), 1000);
    EXPECT_EQ (outcome, Outcome::Abandoned);
    EXPECT_FALSE (job.ran);
    EXPECT_FALSE (job.queued); // revoked: nothing will run it against a pass that has gone
}

// The same shape without the flag is the old Invoke: the join lasts the timeout.
TEST (GateWait, WithoutTheStopFlagTheJoinSitsOutTheTimeout)
{
    Job job;
    const std::atomic<bool> never { false };
    Outcome outcome = Outcome::Completed;
    const Clock::time_point start = Clock::now ();
    std::thread worker ([&] { outcome = WaitOn (job, 200, never); });
    worker.join ();

    EXPECT_GE (MsSince (start), 190);
    EXPECT_EQ (outcome, Outcome::TimedOut);
    EXPECT_TRUE (job.queued); // the caller revokes it after a timeout, not Wait
}

// A dispatched job is running on the main thread, so the main thread is not the
// one joining: it is waited for, and its result is not thrown away.
TEST (GateWait, ADispatchedJobIsWaitedForNotAbandoned)
{
    Job job;
    job.Dispatch ();
    const std::atomic<bool> stop { true };
    std::thread mainThread ([&] {
        std::this_thread::sleep_for (std::chrono::milliseconds (60));
        job.Finish ();
    });
    const Outcome outcome = WaitOn (job, 5000, stop);
    mainThread.join ();

    EXPECT_EQ (outcome, Outcome::Completed);
    EXPECT_TRUE (job.ran);
}

TEST (GateWait, AServedJobCompletesWithoutAStop)
{
    Job job;
    const std::atomic<bool> stop { false };
    std::thread mainThread ([&] {
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
        job.Dispatch ();
        job.Finish ();
    });
    const Outcome outcome = WaitOn (job, 5000, stop);
    mainThread.join ();

    EXPECT_EQ (outcome, Outcome::Completed);
}
