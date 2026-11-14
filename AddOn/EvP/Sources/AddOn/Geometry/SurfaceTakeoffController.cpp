#include "Geometry/SurfaceTakeoffController.hpp"

#include <exception>
#include <utility>

namespace geomsrv::surfacetakeoff {
namespace {
ControllerOutcome Failure (const std::string& message)
{
    return { false, {}, message };
}
std::string ErrorText (const ControllerOutcome& result)
{
    return result.error.empty () ? "Surface calculation returned no result." : result.error;
}
} // namespace

const char* ControllerStateName (ControllerState state)
{
    switch (state) {
        case ControllerState::Empty:
            return "empty";
        case ControllerState::Pending:
            return "pending";
        case ControllerState::Current:
            return "current";
        case ControllerState::Stale:
            return "stale";
        case ControllerState::Refused:
            return "refused";
        case ControllerState::Cancelled:
            return "cancelled";
    }
    return "refused";
}

SurfaceTakeoffController::~SurfaceTakeoffController ()
{
    Shutdown ();
}

bool SurfaceTakeoffController::Current (const std::shared_ptr<const Snapshot>& snapshot)
{
    return snapshot && snapshot->id != 0 && snapshot->captureStamp != 0 && MeshStore::Get ().Current () == snapshot &&
           MeshStore::Get ().CaptureStamp () == snapshot->captureStamp;
}

uint64_t SurfaceTakeoffController::Submit (ControllerRequest request, std::string& error)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (stopping_) {
        error = "Surface calculation controller is stopped.";
        return 0;
    }
    if (!request.execute || !Current (request.snapshot)) {
        error = "A current pinned capture and calculation are required.";
        return 0;
    }
    auto ticket = std::make_shared<Ticket> ();
    ticket->request = std::move (request);
    ticket->generation = ++generation_;
    if (running_)
        running_->cancelled.store (true);
    if (pending_)
        pending_->cancelled.store (true);
    terminal_ = ControllerState::Empty;
    error_.clear ();
    completed_.reset ();
    completion_ = {};
    ready_ = false;
    const uint64_t generation = ticket->generation;
    pending_ = std::move (ticket);
    if (!thread_.joinable ()) {
        try {
            thread_ = std::thread ([this] { ThreadMain (); });
        }
        catch (const std::exception& exception) {
            pending_.reset ();
            error = exception.what ();
            return 0;
        }
    }
    workAvailable_.notify_one ();
    return generation;
}

ControllerView SurfaceTakeoffController::ViewLocked () const
{
    ControllerView view;
    view.generation = generation_;
    view.lastGood = lastGood_;
    view.error = error_;
    if ((running_ && !running_->cancelled.load ()) || pending_)
        view.state = ControllerState::Pending;
    else if (terminal_ != ControllerState::Empty)
        view.state = terminal_;
    else if (lastGood_)
        view.state = Current (lastGood_->snapshot) ? ControllerState::Current : ControllerState::Stale;
    return view;
}

ControllerView SurfaceTakeoffController::Poll ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (ready_) {
        auto ticket = std::move (completed_);
        auto result = std::move (completion_);
        completed_.reset ();
        completion_ = {};
        ready_ = false;
        if (ticket->generation == generation_) {
            if (!Current (ticket->request.snapshot)) {
                terminal_ = ControllerState::Stale;
                error_ = "Capture changed before surface result publication.";
            }
            else if (!result.ok) {
                terminal_ = lastGood_ ? ControllerState::Stale : ControllerState::Refused;
                error_ = ErrorText (result);
            }
            else {
                ControllerResult accepted;
                accepted.generation = ticket->generation;
                accepted.snapshotId = ticket->request.snapshot->id;
                accepted.captureStamp = ticket->request.snapshot->captureStamp;
                accepted.snapshot = ticket->request.snapshot;
                accepted.payload = std::move (result.payload);
                lastGood_ = std::move (accepted);
                terminal_ = ControllerState::Empty;
                error_.clear ();
            }
        }
    }
    return ViewLocked ();
}

bool SurfaceTakeoffController::Cancel (uint64_t generation)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (generation != 0 && generation_ != generation)
        return false;
    ++generation_;
    if (running_)
        running_->cancelled.store (true);
    pending_.reset ();
    completed_.reset ();
    completion_ = {};
    ready_ = false;
    terminal_ = lastGood_ ? ControllerState::Stale : ControllerState::Cancelled;
    error_ = "Surface calculation cancelled.";
    return true;
}

bool SurfaceTakeoffController::Clear (uint64_t generation)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (generation != 0 && generation_ != generation)
        return false;
    ++generation_;
    if (running_)
        running_->cancelled.store (true);
    pending_.reset ();
    completed_.reset ();
    completion_ = {};
    ready_ = false;
    lastGood_.reset ();
    terminal_ = ControllerState::Empty;
    error_.clear ();
    return true;
}

bool SurfaceTakeoffController::CommitPublication (uint64_t generation,
                                                  const std::function<bool (std::string&)>& publish, std::string& error)
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (!publish || generation_ != generation || !lastGood_ || lastGood_->generation != generation ||
        ViewLocked ().state != ControllerState::Current) {
        error = "The requested controller result is not current.";
        return false;
    }
    return publish (error);
}

void SurfaceTakeoffController::OnProjectClosed ()
{
    Clear ();
}

void SurfaceTakeoffController::Shutdown ()
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        ++generation_;
        if (running_)
            running_->cancelled.store (true);
        if (pending_)
            pending_->cancelled.store (true);
        workAvailable_.notify_one ();
    }
    if (thread_.joinable ())
        thread_.join ();
    std::lock_guard<std::mutex> lock (mutex_);
    pending_.reset ();
    running_.reset ();
    completed_.reset ();
    completion_ = {};
    ready_ = false;
    lastGood_.reset ();
    terminal_ = ControllerState::Empty;
    error_.clear ();
}

void SurfaceTakeoffController::ThreadMain ()
{
    for (;;) {
        std::shared_ptr<Ticket> ticket;
        {
            std::unique_lock<std::mutex> lock (mutex_);
            workAvailable_.wait (lock, [this] { return stopping_ || pending_ != nullptr; });
            if (stopping_)
                return;
            ticket = std::move (pending_);
            running_ = ticket;
            if (ticket->cancelled.load () || ticket->generation != generation_) {
                running_.reset ();
                continue;
            }
        }
        ControllerOutcome result;
        try {
            result = ticket->request.execute (ticket->cancelled);
        }
        catch (const std::exception& exception) {
            result = Failure (exception.what ());
        }
        catch (...) {
            result = Failure ("Surface calculation failed.");
        }
        {
            std::lock_guard<std::mutex> lock (mutex_);
            running_.reset ();
            if (ticket->cancelled.load () || stopping_ || ticket->generation != generation_)
                continue;
            completed_ = std::move (ticket);
            completion_ = std::move (result);
            ready_ = true;
        }
    }
}

SurfaceTakeoffController& SharedSurfaceTakeoffController ()
{
    // Process-lifetime owner avoids joining under the loader lock. AddOnMain
    // revokes on document close and explicitly joins on unload.
    static auto* controller = new SurfaceTakeoffController ();
    return *controller;
}

} // namespace geomsrv::surfacetakeoff
