#include "Geometry/SurfaceTakeoffController.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <set>
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

// ModelerAPI::Element::Type, kept SDK-free and checked at the native boundary.
constexpr int32_t kWall = 1, kSlab = 2, kDoor = 13, kWindow = 14, kColumn = 17, kBeam = 19;

const std::map<std::string, std::set<int32_t>> kCoverTypes { { "constructionWithOpenings",
                                                               { kWall, kSlab, kColumn, kBeam, kDoor, kWindow } },
                                                             { "construction", { kWall, kSlab, kColumn, kBeam } },
                                                             { "wallsSlabsBeams", { kWall, kSlab, kBeam } },
                                                             { "slabs", { kSlab } },
                                                             { "wallsSlabs", { kWall, kSlab } },
                                                             { "extraOnly", {} } };

std::string CoverScopeId (const Request& request, const std::string& policy)
{
    // FNV-1a over sorted canonical GUIDs with an unambiguous separator.
    // The explicit GUID list travels with the ID and controls the calculation.
    uint64_t hash = 14695981039346656037ull;
    for (const auto& guid : request.coverGuids) {
        for (const unsigned char byte : guid)
            hash = (hash ^ byte) * 1099511628211ull;
        hash = (hash ^ 0xffu) * 1099511628211ull;
    }
    return "covers-v1:" + policy + ":" + std::to_string (request.snapshotId) + ":" +
           std::to_string (request.captureStamp) + ":" + std::to_string (hash);
}
} // namespace

std::string CanonicalGuid (std::string guid)
{
    if (guid.size () == 38 && guid.front () == '{' && guid.back () == '}')
        guid = guid.substr (1, 36);
    std::transform (guid.begin (), guid.end (), guid.begin (),
                    [] (unsigned char c) { return char (std::toupper (c)); });
    return guid;
}

std::optional<Request> PrepareRequest (const Snapshot& snapshot, const std::string& regionMode,
                                       const std::string& coverPolicy, const std::vector<std::string>& extraCovers,
                                       const std::vector<std::string>& surfaceNames, std::string& error)
{
    const auto fail = [&error] (std::string reason) -> std::optional<Request> {
        error = std::move (reason);
        return std::nullopt;
    };
    const auto coverTypes = kCoverTypes.find (coverPolicy);
    const bool allElements = regionMode == "allElementFaces";
    if ((!allElements && regionMode != "allWallFaces" && regionMode != "seededFacade") ||
        coverTypes == kCoverTypes.end ())
        return fail ("Unsupported target scope or cover policy.");
    if (!snapshot.completeModel || snapshot.id == 0 || snapshot.captureStamp == 0 ||
        snapshot.surfaceAttributes.empty ())
        return fail ("A fresh unfiltered model capture with a project surface catalog is required.");

    std::map<std::string, size_t> inventory;
    std::set<std::string> targets, covers;
    for (const auto& mesh : snapshot.meshes) {
        const auto guid = CanonicalGuid (mesh.guid);
        ++inventory[guid];
        const auto type = mesh.elemType;
        if (type == kWall || (allElements && (type == kSlab || type == kColumn || type == kBeam)))
            targets.insert (guid);
        if (coverTypes->second.contains (type))
            covers.insert (guid);
    }
    for (const auto& guid : extraCovers)
        covers.insert (CanonicalGuid (guid));
    if (targets.empty () || targets.size () > 64 || covers.size () > 128)
        return fail ("Use 1..64 captured candidate elements and at most 128 explicit covering elements.");
    for (const auto& guid : targets)
        if (guid.empty () || inventory.at (guid) != 1)
            return fail ("Target missing or ambiguous in capture: " + guid);
    for (const auto& guid : covers)
        if (guid.empty () || !inventory.contains (guid) || inventory.at (guid) != 1)
            return fail ("Explicit cover missing or ambiguous in capture: " + guid);

    Request request;
    request.snapshotId = snapshot.id;
    request.captureStamp = snapshot.captureStamp;
    request.regionMode = regionMode;
    request.targetGuids.assign (targets.begin (), targets.end ());
    request.coverGuids.assign (covers.begin (), covers.end ());
    request.coverScopeId = CoverScopeId (request, coverPolicy);
    std::set<int32_t> seen;
    for (const auto& name : surfaceNames) {
        if (name.empty ())
            continue;
        int32_t found = 0;
        for (const auto& [index, surface] : snapshot.surfaceAttributes) {
            if (surface.name != name)
                continue;
            if (found != 0)
                return fail ("Selected project surface is ambiguous in the fresh capture: " + name);
            found = index;
        }
        if (found <= 0)
            return fail ("Selected project surface is absent in the fresh capture: " + name);
        if (seen.insert (found).second)
            request.surfaceAttributeIndices.push_back (found);
    }
    error.clear ();
    return request;
}

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
    return PollLocked ();
}

std::optional<ControllerView> SurfaceTakeoffController::TryPoll ()
{
    if (publishing_.load ())
        return std::nullopt;
    const std::unique_lock<std::mutex> lock (mutex_, std::try_to_lock);
    if (!lock)
        return std::nullopt;
    return PollLocked ();
}

ControllerView SurfaceTakeoffController::PollLocked ()
{
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
            else if (!result.ok || !result.payload) {
                terminal_ = lastGood_          ? ControllerState::Stale
                            : result.cancelled ? ControllerState::Cancelled
                                               : ControllerState::Refused;
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
    struct PublicationScope {
        std::atomic<bool>& active;
        ~PublicationScope ()
        {
            active.store (false);
        }
    } scope { publishing_ };
    publishing_.store (true);
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
