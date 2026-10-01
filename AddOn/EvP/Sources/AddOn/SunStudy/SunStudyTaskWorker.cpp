#include "SunStudy/SunStudyTaskWorker.hpp"
#include "SunStudy/SunStudyThreadPolicy.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace evp::sunstudy {

SunStudyTaskWorker::~SunStudyTaskWorker ()
{
    Shutdown ();
}

bool SunStudyTaskWorker::Submit (StudyTaskRequest request, std::string& error)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (stopping_ || active_ != nullptr || !request.execute) {
        error = stopping_ ? "sun study task worker is stopped" : "sun study task worker is busy or has no task";
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

bool SunStudyTaskWorker::Poll (StudyTaskCompletion& completion)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (!ready_ || active_->cancelled.load ())
        return false;
    completion = std::move (completion_);
    ready_ = false;
    active_.reset ();
    return true;
}

bool SunStudyTaskWorker::Busy () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return active_ != nullptr;
}

void SunStudyTaskWorker::Cancel ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (active_ != nullptr)
        active_->cancelled.store (true);
    workAvailable_.notify_one ();
}

void SunStudyTaskWorker::Shutdown ()
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        stopping_ = true;
        if (active_ != nullptr)
            active_->cancelled.store (true);
        workAvailable_.notify_one ();
    }
    if (thread_.joinable ())
        thread_.join ();
}

void SunStudyTaskWorker::Discard (const std::shared_ptr<Ticket>& ticket)
{
    if (ticket->discarded)
        return;
    ticket->discarded = true;
    try {
        if (ticket->request.discard)
            ticket->request.discard ();
    }
    catch (...) {
        // Cleanup cannot terminate the mailbox or escape quit/unload.
    }
}

void SunStudyTaskWorker::ThreadMain ()
{
    const bool priorityLowered = UseInteractiveStudyPriority ();
    for (;;) {
        std::shared_ptr<Ticket> ticket;
        bool discardOnly = false;
        {
            std::unique_lock<std::mutex> lock (mutex_);
            workAvailable_.wait (
                lock, [this] () { return stopping_ || pending_ != nullptr || (ready_ && active_->cancelled.load ()); });
            if (pending_ != nullptr)
                ticket = std::move (pending_);
            else if (ready_)
                ticket = active_;
            else if (stopping_)
                return;
            discardOnly = ticket->cancelled.load () || stopping_;
        }
        StudyTaskCompletion completion;
        completion.sessionGeneration = ticket->request.sessionGeneration;
        completion.runGeneration = ticket->request.runGeneration;
        completion.threadId = std::this_thread::get_id ();
        completion.priorityLowered = priorityLowered;
        const auto started = std::chrono::steady_clock::now ();
        if (!discardOnly) {
            try {
                ticket->request.execute (ticket->cancelled);
            }
            catch (const std::exception& exception) {
                completion.error = exception.what ();
            }
            catch (...) {
                completion.error = "sun study task failed";
            }
        }
        completion.wallMilliseconds =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
        std::unique_lock<std::mutex> lock (mutex_);
        if (ticket->cancelled.load () || stopping_ || !completion.error.empty ()) {
            lock.unlock ();
            Discard (ticket);
            lock.lock ();
        }
        if (ticket->cancelled.load () || stopping_) {
            active_.reset ();
            ready_ = false;
            if (stopping_)
                return;
        }
        else {
            completion_ = std::move (completion);
            ready_ = true;
        }
    }
}

} // namespace evp::sunstudy
