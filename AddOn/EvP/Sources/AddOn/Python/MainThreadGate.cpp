#include "APIEnvir.h"
#include "ACAPinc.h"

#include "MainThreadGate.hpp"
#include "ResourceMDIDIds.hpp"   // AC_MDID_DEV / AC_MDID_LOC, parsed from AddOnFix.grc

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace {

// One generic command carries every job: the payload is the queued std::function,
// so the gate never needs a new ModulCommand per operation.
constexpr GSType GateJobCmdID     = 'EGTJ';
constexpr Int32  GateCmdVersion   = 1L;

std::thread::id mainThreadId;

struct Completion {
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;

    void Complete ()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            done = true;
        }
        cv.notify_all ();
    }

    bool WaitFor (int timeoutMs)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return cv.wait_for (lock, std::chrono::milliseconds (timeoutMs), [this] { return done; });
    }
};

struct Job {
    std::function<void ()>      fn;
    std::shared_ptr<Completion> completion;   // null == fire-and-forget
};

std::mutex     queueMutex;
std::deque<Job> jobQueue;

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
        jobQueue.push_back (std::move (job));
    }

    const API_ModulID mdid = { AC_MDID_DEV, AC_MDID_LOC };   // ourselves
    const GSErrCode   err  = ACAPI_AddOnAddOnCommunication_CallFromEventLoop (
                                 &mdid, GateJobCmdID, GateCmdVersion, nullptr, true, nullptr);
    if (err != NoError) {
        // Drop the job we just queued: nothing will ever dispatch it.
        std::lock_guard<std::mutex> lock (queueMutex);
        if (!jobQueue.empty ())
            jobQueue.pop_back ();
        error = GS::UniString::Printf ("MainThreadGate: CallFromEventLoop failed to post (err=%d).", (int) err);
        return false;
    }
    return true;
}

}   // namespace

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
}

bool MainThreadGate::IsMainThread () const
{
    return std::this_thread::get_id () == mainThreadId;
}

bool MainThreadGate::Invoke (const std::function<void ()>& fn, int timeoutMs, GS::UniString& error)
{
    // Already on the main thread: run inline. Posting and then waiting here
    // would block the very event loop that has to dispatch the job — a
    // guaranteed self-deadlock.
    if (IsMainThread ()) {
        fn ();
        return true;
    }

    auto completion = std::make_shared<Completion> ();
    if (!PostJob (Job { fn, completion }, error))
        return false;

    if (!completion->WaitFor (timeoutMs)) {
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
        error = GS::UniString::Printf (
            "MainThreadGate: timed out after %d ms — posted but never dispatched (%s). "
            "The main event loop is blocked (a modal dialog, or a live user-input pick).",
            timeoutMs, revoked ? "job revoked" : "job already in flight");
        return false;
    }
    return true;
}

bool MainThreadGate::Post (const std::function<void ()>& fn, GS::UniString& error)
{
    if (IsMainThread ()) {
        fn ();
        return true;
    }
    return PostJob (Job { fn, nullptr }, error);
}

}   // namespace evp
