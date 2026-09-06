#include "APIEnvir.h"
#include "ACAPinc.h"

#include "MainThreadGate.hpp"
#include "ResourceMDIDIds.hpp" // AC_MDID_DEV / AC_MDID_LOC, parsed from AddOnFix.grc

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace {

// One generic command carries every job: the payload is the queued std::function,
// so the gate never needs a new ModulCommand per operation.
constexpr GSType GateJobCmdID = 'EGTJ';
constexpr Int32 GateCmdVersion = 1L;

std::thread::id mainThreadId;

struct Completion {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool cancelled = false;

    void Complete ()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            done = true;
        }
        cv.notify_all ();
    }

    void Cancel ()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            cancelled = true;
            done = true;
        }
        cv.notify_all ();
    }

    bool WasCancelled ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return cancelled;
    }

    bool WaitFor (int timeoutMs)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return cv.wait_for (lock, std::chrono::milliseconds (timeoutMs), [this] { return done; });
    }
};

struct Job {
    std::function<void ()> fn;
    std::shared_ptr<Completion> completion; // null == fire-and-forget
    // When it was queued, so the wait it actually suffered is measurable rather
    // than guessed from the outside.
    int64_t postedMs = 0;
};

std::mutex queueMutex;
std::deque<Job> jobQueue;
bool shuttingDown = false;

// ---- the dispatch counters ------------------------------------------------
// Atomics, so a worker thread can read them while the main thread is wedged -
// which is the one moment they matter. See MainThreadGate::Stats.
std::atomic<uint64_t> statPosted { 0 };
std::atomic<uint64_t> statDispatched { 0 };
std::atomic<uint64_t> statInline { 0 };
std::atomic<uint64_t> statTimeouts { 0 };
std::atomic<uint64_t> statPostFailures { 0 };
std::atomic<int64_t> statLastDispatchMs { 0 };
std::atomic<int64_t> statLastPostMs { 0 };
std::atomic<int64_t> statLongestWaitMs { 0 };

// The command running on the main thread right now. A mutex rather than an
// atomic because it is a string; held only for a copy, and never by the gate's
// dispatch path, so a reader cannot be blocked by the thread it is measuring.
std::mutex commandMutex;
std::string mainThreadCommand;
int64_t mainThreadCommandStartedMs = 0;

int64_t NowMs ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now ().time_since_epoch ())
        .count ();
}

// Runs on the MAIN thread. One posted command consumes exactly one job, so
// ordering is preserved and a job can never be run twice.
GSErrCode GateJobHandler (GSHandle /*params*/, GSPtr /*resultData*/, bool /*silentMode*/)
{
    Job job;
    {
        std::lock_guard<std::mutex> lock (queueMutex);
        if (jobQueue.empty ())
            return NoError;
        job = std::move (jobQueue.front ());
        jobQueue.pop_front ();
    }

    const int64_t dispatchedMs = NowMs ();
    statDispatched.fetch_add (1, std::memory_order_relaxed);
    statLastDispatchMs.store (dispatchedMs, std::memory_order_relaxed);
    if (job.postedMs != 0) {
        const int64_t waited = dispatchedMs - job.postedMs;
        int64_t longest = statLongestWaitMs.load (std::memory_order_relaxed);
        while (waited > longest &&
               !statLongestWaitMs.compare_exchange_weak (longest, waited, std::memory_order_relaxed)) {
        }
    }

    if (job.fn)
        job.fn ();

    // Complete AFTER fn: Invoke's contract is "returns once the work is done".
    if (job.completion != nullptr)
        job.completion->Complete ();
    return NoError;
}

bool PostJob (Job&& job, GS::UniString& error)
{
    {
        std::lock_guard<std::mutex> lock (queueMutex);
        if (shuttingDown) {
            error = "MainThreadGate: the add-on is shutting down.";
            return false;
        }
        job.postedMs = NowMs ();
        jobQueue.push_back (std::move (job));
    }
    statPosted.fetch_add (1, std::memory_order_relaxed);
    statLastPostMs.store (NowMs (), std::memory_order_relaxed);

    const API_ModulID mdid = { AC_MDID_DEV, AC_MDID_LOC }; // ourselves
    const GSErrCode err =
        ACAPI_AddOnAddOnCommunication_CallFromEventLoop (&mdid, GateJobCmdID, GateCmdVersion, nullptr, true, nullptr);
    if (err != NoError) {
        // Drop the job we just queued: nothing will ever dispatch it.
        std::lock_guard<std::mutex> lock (queueMutex);
        if (!jobQueue.empty ())
            jobQueue.pop_back ();
        statPostFailures.fetch_add (1, std::memory_order_relaxed);
        error = GS::UniString::Printf ("MainThreadGate: CallFromEventLoop failed to post (err=%d).", (int) err);
        return false;
    }
    return true;
}

bool IsShuttingDown ()
{
    std::lock_guard<std::mutex> lock (queueMutex);
    return shuttingDown;
}

} // namespace

namespace evp {

MainThreadGate& MainThreadGate::Get ()
{
    static MainThreadGate instance;
    return instance;
}

GSErrCode MainThreadGate::RegisterServices ()
{
    return ACAPI_AddOnAddOnCommunication_RegisterSupportedService (GateJobCmdID, GateCmdVersion);
}

GSErrCode MainThreadGate::InstallHandlers ()
{
    return ACAPI_AddOnIntegration_InstallModulCommandHandler (GateJobCmdID, GateCmdVersion, GateJobHandler);
}

void MainThreadGate::RecordMainThread ()
{
    mainThreadId = std::this_thread::get_id ();
    std::lock_guard<std::mutex> lock (queueMutex);
    shuttingDown = false;
}

bool MainThreadGate::IsMainThread () const
{
    return std::this_thread::get_id () == mainThreadId;
}

void MainThreadGate::BeginShutdown ()
{
    std::deque<Job> queued;
    {
        std::lock_guard<std::mutex> lock (queueMutex);
        shuttingDown = true;
        queued.swap (jobQueue);
    }
    for (Job& job : queued) {
        if (job.completion != nullptr)
            job.completion->Cancel ();
    }
}

bool MainThreadGate::Invoke (const std::function<void ()>& fn, int timeoutMs, GS::UniString& error)
{
    if (IsShuttingDown ()) {
        error = "MainThreadGate: the add-on is shutting down.";
        return false;
    }

    // Already on the main thread: run inline. Posting and then waiting here
    // would block the very event loop that has to dispatch the job — a
    // guaranteed self-deadlock.
    if (IsMainThread ()) {
        statInline.fetch_add (1, std::memory_order_relaxed);
        fn ();
        return true;
    }

    auto completion = std::make_shared<Completion> ();
    if (!PostJob (Job { fn, completion }, error))
        return false;

    if (!completion->WaitFor (timeoutMs)) {
        statTimeouts.fetch_add (1, std::memory_order_relaxed);
        // Best-effort revoke: if the job is still queued, drop it so it cannot
        // run against a caller that has already given up. Losing this race means
        // the job is executing (or has just executed) on the main thread, which
        // is why `fn` must capture by value — see the header's contract.
        bool revoked = false;
        {
            std::lock_guard<std::mutex> lock (queueMutex);
            for (auto it = jobQueue.begin (); it != jobQueue.end (); ++it) {
                if (it->completion == completion) {
                    jobQueue.erase (it);
                    revoked = true;
                    break;
                }
            }
        }
        error = GS::UniString::Printf ("MainThreadGate: timed out after %d ms — posted but never dispatched (%s). "
                                       "The main event loop is blocked (a modal dialog, or a live user-input pick).",
                                       timeoutMs, revoked ? "job revoked" : "job already in flight");
        return false;
    }
    if (completion->WasCancelled ()) {
        error = "MainThreadGate: the add-on shut down before the job was dispatched.";
        return false;
    }
    return true;
}

bool MainThreadGate::Post (const std::function<void ()>& fn, GS::UniString& error)
{
    if (IsShuttingDown ()) {
        error = "MainThreadGate: the add-on is shutting down.";
        return false;
    }
    if (IsMainThread ()) {
        fn ();
        return true;
    }
    return PostJob (Job { fn, nullptr }, error);
}

MainThreadGate::Stats MainThreadGate::Snapshot () const
{
    Stats stats;
    stats.posted = statPosted.load (std::memory_order_relaxed);
    stats.dispatched = statDispatched.load (std::memory_order_relaxed);
    stats.inlineRuns = statInline.load (std::memory_order_relaxed);
    stats.timeouts = statTimeouts.load (std::memory_order_relaxed);
    stats.postFailures = statPostFailures.load (std::memory_order_relaxed);
    stats.longestWaitMs = statLongestWaitMs.load (std::memory_order_relaxed);

    const int64_t now = NowMs ();
    const int64_t lastDispatch = statLastDispatchMs.load (std::memory_order_relaxed);
    const int64_t lastPost = statLastPostMs.load (std::memory_order_relaxed);
    stats.msSinceLastDispatch = lastDispatch == 0 ? -1 : now - lastDispatch;
    stats.msSinceLastPost = lastPost == 0 ? -1 : now - lastPost;

    {
        std::lock_guard<std::mutex> lock (queueMutex);
        stats.queueDepth = jobQueue.size ();
        stats.shuttingDown = shuttingDown;
    }
    stats.mainThreadKnown = mainThreadId != std::thread::id {};
    {
        std::lock_guard<std::mutex> lock (commandMutex);
        stats.mainThreadCommand = mainThreadCommand;
        stats.mainThreadCommandMs = mainThreadCommand.empty () ? -1 : now - mainThreadCommandStartedMs;
    }
    return stats;
}

void MainThreadGate::NoteMainThreadCommand (const char* name)
{
    std::lock_guard<std::mutex> lock (commandMutex);
    if (name == nullptr || *name == '\0') {
        mainThreadCommand.clear ();
        mainThreadCommandStartedMs = 0;
        return;
    }
    mainThreadCommand = name;
    mainThreadCommandStartedMs = NowMs ();
}

} // namespace evp
