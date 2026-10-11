#include "NativeCommands/PolygonPickState.hpp"

#include <cmath>

namespace geomsrv {
namespace polygonpick {

namespace {

// Field-for-field clear of one REQUEST. Deliberately does not touch the physical
// in-flight flag: that belongs to the SDK call, not to the request record.
void ClearRequestLocked (std::string& requestId, Status& status, bool& cancelRequested, uint64_t& requestGeneration,
                         WorldIdentity& expected, bool& callbackSawComponent, Sighting& sighting, bool& haveSdkCode,
                         long& sdkCode, std::string& reason, FacePick& pick, bool& callbackMatchedPick)
{
    requestId.clear ();
    status = Status::Idle;
    cancelRequested = false;
    requestGeneration = 0;
    expected = WorldIdentity {};
    callbackSawComponent = false;
    sighting = Sighting {};
    haveSdkCode = false;
    sdkCode = 0;
    reason.clear ();
    pick = FacePick {};
    callbackMatchedPick = false;
}

bool IsFiniteNormal (const double n[3])
{
    return std::isfinite (n[0]) && std::isfinite (n[1]) && std::isfinite (n[2]) &&
           (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) > 1e-12;
}

} // namespace

BeginStatus PolygonPickState::Begin (const std::string& newRequestId, const WorldIdentity& world, Token& outToken)
{
    std::lock_guard<std::mutex> lock (mtx);

    // Both refusals are deliberate. A queued request that has not reached the SDK
    // is still owned by a live token, and a physically running GetPoint cannot be
    // superseded at all (the modal loop does not yield to a new job).
    if (inputInFlight)
        return BeginStatus::BusyInput;
    if (status == Status::Pending || status == Status::Running)
        return BeginStatus::BusyRequest;

    ++serial;
    requestId = newRequestId;
    status = Status::Pending;
    cancelRequested = false;
    requestGeneration = generation;
    expected = world;
    callbackSawComponent = false;
    lastSighting = Sighting {};
    haveSdkCode = false;
    sdkCode = 0;
    reason.clear ();
    pick = FacePick {};
    callbackMatchedPick = false;

    outToken.requestId = requestId;
    outToken.serial = serial;
    outToken.generation = generation;
    outToken.expected = expected;
    return BeginStatus::Accepted;
}

bool PolygonPickState::RequestCancel (const std::string& id, Token& outToken)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (id != requestId)
        return false;
    // A terminal request is left EXACTLY as it is -- a cancel after Done must not
    // flip cancelRequested on the outcome the caller is about to read.
    if (status != Status::Pending && status != Status::Running)
        return false;

    cancelRequested = true;
    outToken.requestId = requestId;
    outToken.serial = serial;
    outToken.generation = generation;
    outToken.expected = expected;

    if (status == Status::Pending) {
        status = Status::Cancelled;
        reason = "cancel requested before the pick started";
    }
    return true;
}

InputStart PolygonPickState::BeginInput (const Token& token, const WorldIdentity& observed)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (token.generation != generation)
        return InputStart::LifecycleChanged;
    if (token.serial != serial)
        return InputStart::Superseded;
    if (status == Status::Cancelled)
        return InputStart::Cancelled;
    if (status != Status::Pending)
        return InputStart::NotOwned;

    if (cancelRequested) {
        status = Status::Cancelled;
        reason = "cancel requested before the pick started";
        return InputStart::Cancelled;
    }
    if (observed != expected) {
        status = Status::Failed;
        haveSdkCode = false;
        sdkCode = 0;
        reason = "the model or snapshot changed before the pick started";
        return InputStart::StaleModel;
    }

    status = Status::Running;
    inputInFlight = true;
    inputSerial = serial;
    return InputStart::Started;
}

void PolygonPickState::EndInput (const Token& token)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (inputInFlight && token.serial == inputSerial)
        inputInFlight = false;
}

Abort PolygonPickState::ShouldAbort (const Token& token) const
{
    std::lock_guard<std::mutex> lock (mtx);
    if (token.generation != generation)
        return Abort::LifecycleChanged;
    if (token.serial != serial)
        return Abort::Superseded;
    if (cancelRequested)
        return Abort::CancelRequested;
    return Abort::None;
}

void PolygonPickState::OnComponent (const Token& token, const Sighting& sighting)
{
    std::lock_guard<std::mutex> lock (mtx);
    // Exact token AND a live in-flight input. A callback that arrives after Reset,
    // after the input ended, or for a superseded request is dropped entirely.
    if (!OwnsLocked (token) || status != Status::Running || !inputInFlight || token.serial != inputSerial)
        return;
    callbackSawComponent = sighting.valid;
    lastSighting = sighting;
}

void PolygonPickState::Complete (const Token& token, FacePick committed, const WorldIdentity& observed)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (!OwnsLocked (token) || status != Status::Running)
        return;

    // Cancellation can arrive after RunPick's last unlocked abort check. Make
    // it win atomically here rather than publish success from a cancelled token.
    if (cancelRequested) {
        status = Status::Cancelled;
        haveSdkCode = false;
        sdkCode = 0;
        reason = "cancel requested before the pick result was published";
        return;
    }

    if (observed != expected) {
        status = Status::Failed;
        haveSdkCode = false;
        sdkCode = 0;
        reason = "the model or snapshot changed while the pick was running";
        return;
    }

    // The normal is attached ONLY when the last sighting names the exact same
    // component as the committed pick (guid + dbElemIdx + bodyIdx + ipgon) and the
    // normal is finite and non-zero. A stale hover, or a same-GUID different-body
    // sighting, contributes nothing.
    callbackMatchedPick = committed.found && callbackSawComponent && lastSighting.valid &&
                          lastSighting.guid == committed.guid && lastSighting.dbElemIdx == committed.dbElemIdx &&
                          lastSighting.bodyIdx == committed.bodyIdx && lastSighting.ipgon == committed.ipgon &&
                          IsFiniteNormal (lastSighting.normal);
    if (callbackMatchedPick) {
        committed.haveNormal = true;
        committed.normal[0] = lastSighting.normal[0];
        committed.normal[1] = lastSighting.normal[1];
        committed.normal[2] = lastSighting.normal[2];
    }

    pick = committed;
    status = Status::Done;
}

void PolygonPickState::Fail (const Token& token, long code, const std::string& why)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (!OwnsLocked (token) || (status != Status::Pending && status != Status::Running))
        return;
    status = Status::Failed;
    haveSdkCode = (code != 0);
    sdkCode = code;
    reason = why;
}

void PolygonPickState::Cancelled (const Token& token, long code, const std::string& why)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (!OwnsLocked (token) || (status != Status::Pending && status != Status::Running))
        return;
    status = Status::Cancelled;
    haveSdkCode = (code != 0);
    sdkCode = code;
    reason = why;
}

Outcome PolygonPickState::Poll (const std::string& id, const WorldIdentity& observed)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (id != requestId)
        return Outcome {};

    Outcome outcome = BuildOutcomeLocked ();

    // A Done result is only valid while the world identity it was captured under
    // still holds; a drift between the input and the poll fails it here.
    if (outcome.status == Status::Done && observed != expected) {
        outcome.status = Status::Failed;
        outcome.haveSdkCode = false;
        outcome.sdkCode = 0;
        outcome.reason = "the model or snapshot changed before the pick result was read";
    }

    if (outcome.status == Status::Done || outcome.status == Status::Cancelled || outcome.status == Status::Failed) {
        // Consumed once. The serial is bumped so the consumed token no longer owns
        // anything -- a late Complete/Fail for it can only be a no-op.
        ++serial;
        ClearRequestLocked (requestId, status, cancelRequested, requestGeneration, expected, callbackSawComponent,
                            lastSighting, haveSdkCode, sdkCode, reason, pick, callbackMatchedPick);
    }
    return outcome;
}

Outcome PolygonPickState::Peek (const std::string& id) const
{
    std::lock_guard<std::mutex> lock (mtx);
    if (id != requestId)
        return Outcome {};
    return BuildOutcomeLocked ();
}

void PolygonPickState::Reset ()
{
    std::lock_guard<std::mutex> lock (mtx);
    ++generation;
    ClearRequestLocked (requestId, status, cancelRequested, requestGeneration, expected, callbackSawComponent,
                        lastSighting, haveSdkCode, sdkCode, reason, pick, callbackMatchedPick);
    // inputInFlight is intentionally NOT cleared: the SDK call is still running.
}

uint64_t PolygonPickState::Generation () const
{
    std::lock_guard<std::mutex> lock (mtx);
    return generation;
}

bool PolygonPickState::IsActive (const Token& token) const
{
    std::lock_guard<std::mutex> lock (mtx);
    return OwnsLocked (token);
}

bool PolygonPickState::IsRunning (const Token& token) const
{
    std::lock_guard<std::mutex> lock (mtx);
    return OwnsLocked (token) && status == Status::Running && inputInFlight && token.serial == inputSerial;
}

bool PolygonPickState::InputInFlight () const
{
    std::lock_guard<std::mutex> lock (mtx);
    return inputInFlight;
}

Outcome PolygonPickState::BuildOutcomeLocked () const
{
    Outcome outcome;
    outcome.status = status;
    outcome.cancelRequested = cancelRequested;
    outcome.running = (status == Status::Running);
    outcome.callbackSawComponent = callbackSawComponent;
    outcome.callbackMatchedPick = callbackMatchedPick;
    outcome.haveSdkCode = haveSdkCode;
    outcome.sdkCode = sdkCode;
    outcome.reason = reason;
    outcome.pick = pick;
    outcome.sessionGeneration = requestGeneration;
    outcome.expected = expected;
    return outcome;
}

bool PolygonPickState::OwnsLocked (const Token& token) const
{
    // Serial AND generation. Reset() bumps the generation so a callback/complete
    // from a closed document is never owned; a consume bumps the serial so a late
    // result for an already-answered request is a no-op.
    return token.serial == serial && token.generation == generation;
}

} // namespace polygonpick
} // namespace geomsrv
