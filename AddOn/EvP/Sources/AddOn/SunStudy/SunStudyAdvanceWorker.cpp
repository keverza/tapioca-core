#include "SunStudy/SunStudyAdvanceWorker.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyThreadPolicy.hpp"

#include <exception>
#include <chrono>
#include <utility>

namespace evp::sunstudy {

SunStudyAdvanceWorker::~SunStudyAdvanceWorker ()
{
    Shutdown ();
}

bool SunStudyAdvanceWorker::Submit (AdvanceRequest request, std::string& error)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (stopping_ || active_ != nullptr) {
        error = stopping_ ? "sun study worker is stopped" : "sun study worker is busy";
        return false;
    }
    const uint64_t revision = SunStudyStore::Get ().Revision (request.studyId);
    if (request.recordRevision != 0 && request.recordRevision != revision) {
        error = "sun study record changed";
        return false;
    }
    request.recordRevision = revision;
    if (request.recordRevision == 0 || request.maxSteps == 0) {
        error = "sun study worker needs a live study and a nonzero step budget";
        return false;
    }
    auto ticket = std::make_shared<Ticket> ();
    ticket->request = std::move (request);
    if (!thread_.joinable ()) {
        try {
            thread_ = std::thread ([this] () { ThreadMain (); });
        }
        catch (const std::exception& exception) {
            error = exception.what ();
            return false;
        }
    }
    active_ = pending_ = std::move (ticket);
    workAvailable_.notify_one ();
    return true;
}

bool SunStudyAdvanceWorker::Poll (AdvanceCompletion& completion)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (!ready_)
        return false;
    completion = std::move (completion_);
    ready_ = false;
    active_.reset ();
    return true;
}

bool SunStudyAdvanceWorker::Busy () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return active_ != nullptr;
}

void SunStudyAdvanceWorker::Cancel ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (active_ == nullptr)
        return;
    active_->cancelled.store (true);
    if (ready_ || pending_ != nullptr) {
        pending_.reset ();
        active_.reset ();
    }
    ready_ = false;
    completion_ = AdvanceCompletion {};
}

void SunStudyAdvanceWorker::Shutdown ()
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        stopping_ = true;
        if (active_ != nullptr)
            active_->cancelled.store (true);
        pending_.reset ();
        ready_ = false;
        workAvailable_.notify_one ();
    }
    if (thread_.joinable ())
        thread_.join ();
    std::lock_guard<std::mutex> lock (mutex_);
    active_.reset ();
    completion_ = AdvanceCompletion {};
}

AdvanceCompletion SunStudyAdvanceWorker::Execute (const std::shared_ptr<Ticket>& ticket)
{
    AdvanceCompletion completion;
    completion.request = ticket->request;
    completion.threadId = std::this_thread::get_id ();
    const AdvanceRequest& request = ticket->request;
    const auto started = std::chrono::steady_clock::now ();
    for (size_t step = 0; step < request.maxSteps && !ticket->cancelled.load (); ++step) {
        size_t advanced = 0;
        auto& store = SunStudyStore::Get ();
        if (!store.Advance (request.studyId, 1, request.maxParallel, request.tmin, request.tmax, advanced,
                            completion.error, &ticket->cancelled, request.recordRevision))
            return completion;
        completion.advanced += advanced;
        if (!store.Progress (request.studyId, completion.progress, completion.error, request.recordRevision))
            return completion;
        if (advanced == 0 || completion.progress.converged || completion.progress.empty)
            break;
        if (request.maxMilliseconds > 0.0 &&
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count () >=
                request.maxMilliseconds)
            break;
    }
    completion.succeeded = true;
    return completion;
}

void SunStudyAdvanceWorker::ThreadMain ()
{
    const bool priorityLowered = UseInteractiveStudyPriority ();
    for (;;) {
        std::shared_ptr<Ticket> ticket;
        {
            std::unique_lock<std::mutex> lock (mutex_);
            workAvailable_.wait (lock, [this] () { return stopping_ || pending_ != nullptr; });
            if (stopping_)
                return;
            ticket = std::move (pending_);
        }
        AdvanceCompletion completion;
        const auto started = std::chrono::steady_clock::now ();
        try {
            completion = Execute (ticket);
        }
        catch (const std::exception& exception) {
            completion.request = ticket->request;
            completion.error = exception.what ();
        }
        catch (...) {
            completion.request = ticket->request;
            completion.error = "sun study worker failed";
        }
        completion.threadId = std::this_thread::get_id ();
        completion.priorityLowered = priorityLowered;
        completion.wallMilliseconds =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
        {
            std::lock_guard<std::mutex> lock (mutex_);
            if (ticket->cancelled.load () || stopping_) {
                active_.reset ();
                continue;
            }
            completion.readyAt = std::chrono::steady_clock::now ();
            completion_ = std::move (completion);
            ready_ = true;
        }
        NotifyStudyReady (ticket->request.onReady);
    }
}

} // namespace evp::sunstudy
