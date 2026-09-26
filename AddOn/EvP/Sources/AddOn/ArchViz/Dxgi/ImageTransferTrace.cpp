// ArchViz/Dxgi/ImageTransferTrace -- follows a verified scene image toward the
// nominated back buffer, per bounded capture, from inside PassProvenance's and
// SceneCameraPairing's own already-guarded handlers.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: it must
// never gate camera selection, composition or lineage classification, and a
// bound SRV is reported only as a candidate input, never as proven sampling.

#include "ArchViz/Dxgi/ImageTransferTrace.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace imagetransfer {

namespace {

enum class SlotState : uint32_t { Empty = 0, Open = 1, Closed = 2 };

// One tracked resource: the root S plus anything a recorded operation derived
// from it. Context-thread-only plain data -- see the header's threading note.
struct TrackedSet {
    uint64_t resources[kTrackedPerCapture] = {};
    uint64_t parents[kTrackedPerCapture] = {};
    uint32_t count = 0;
};

// One event slot, published reserve/fill/publish like
// `scenecamerapairing::PublishRow` -- every field its own atomic because two
// roles (context thread, Present thread) can reserve into the SAME open
// capture concurrently, at different indices.
struct EventSlot {
    std::atomic<uint32_t> published { 0 };
    std::atomic<uint64_t> epoch { 0 };
    std::atomic<uint32_t> capture { 0 };
    std::atomic<uint64_t> orderSerial { 0 };
    std::atomic<uint32_t> role { uint32_t (Role::Context) };
    std::atomic<uint32_t> kind { uint32_t (Kind::Root) };
    std::atomic<uint32_t> drawKind { uint32_t (passprovenance::DrawKind::Unknown) };
    std::atomic<uint32_t> drawCount { 0 };
    std::atomic<uint64_t> drawsSinceRoot { 0 };
    std::atomic<uint64_t> scenePass { 0 };
    std::atomic<uint64_t> rtv0 { 0 };
    std::atomic<uint32_t> rtvCount { 0 };
    std::atomic<uint64_t> srv0 { 0 };
    std::atomic<uint64_t> srv1 { 0 };
    std::atomic<uint64_t> srv2 { 0 };
    std::atomic<uint64_t> srv3 { 0 };
    std::atomic<uint32_t> trackedSrvMask { 0 };
    std::atomic<uint32_t> trackedSrvHits { 0 };
    std::atomic<uint64_t> source { 0 };
    std::atomic<uint64_t> destination { 0 };
    std::atomic<uint64_t> resource { 0 };
    std::atomic<uint64_t> backBuffer { 0 };
    std::atomic<bool> writesBackBuffer { false };
    std::atomic<bool> readsTracked { false };
    std::atomic<bool> readsRoot { false };
    std::atomic<uint64_t> trackedResource { 0 };
    std::atomic<uint32_t> trackedAdded { 0 };
    std::atomic<uint64_t> parentResource { 0 };
    std::atomic<bool> succeeded { false };
};

// One capture slot. `openOrderSerial` and `closeOrderSerial` are the publish
// gates (0 == not yet safe to read): the opener/closer writes every other
// field in this group first, THEN releases the gate. `state` is a SEPARATE
// atomic used purely for close arbitration (§ threading note in .hpp) -- a
// reader never inspects it directly, only the gates.
struct CaptureSlot {
    std::atomic<uint32_t> state { uint32_t (SlotState::Empty) };
    uint64_t rootScenePass = 0; // context-thread-only until openOrderSerial publishes it
    uint64_t rootResource = 0;
    uint64_t rootEventSerial = 0;
    std::atomic<uint64_t> openOrderSerial { 0 };
    std::atomic<uint64_t> closeOrderSerial { 0 };
    std::atomic<uint32_t> closeReason { uint32_t (CloseReason::Open) };
    std::atomic<uint32_t> presentsSeen { 0 };   // Present thread writes, context thread never touches
    std::atomic<uint64_t> drawsSinceRoot { 0 }; // context thread writes, Present thread never touches
    std::atomic<uint64_t> rootWrites { 0 };
    std::atomic<uint64_t> unmodelledWork { 0 };
    std::atomic<uint32_t> trackedCount { 0 };
    std::atomic<uint64_t> trackedOverflow { 0 };
    std::atomic<uint32_t> eventsReserved { 0 }; // shared reservation counter, both roles fetch_add into it
    std::atomic<uint32_t> eventsRecorded { 0 };
    std::atomic<uint64_t> eventsDropped { 0 };
};

std::atomic<bool> g_enabled { false };
std::atomic<uint64_t> g_epoch { 0 };
std::atomic<uint64_t> g_orderSerial { 0 }; // ONE global serial shared by context and Present roles
std::atomic<uint32_t> g_activeCapture { uint32_t (kMaxCaptures) }; // sentinel kMaxCaptures == none open
std::atomic<uint64_t> g_backBuffer { 0 };                          // published on every nominated Present while enabled

std::atomic<uint32_t> g_capturesOpened { 0 };
std::atomic<uint32_t> g_capturesClosed { 0 };
std::atomic<uint32_t> g_capturesTruncated { 0 };
std::atomic<uint32_t> g_capturesAbortedByResize { 0 };
std::atomic<uint64_t> g_rootsSeen { 0 };
std::atomic<uint64_t> g_eventsRecorded { 0 };
std::atomic<uint64_t> g_eventsDropped { 0 };
std::atomic<uint64_t> g_eventsAfterClose { 0 };
std::atomic<uint64_t> g_unmodelledWork { 0 };
std::atomic<uint64_t> g_trackedOverflowTotal { 0 };

CaptureSlot g_captures[kMaxCaptures];
TrackedSet g_tracked[kMaxCaptures]; // context-thread-only, never touched by the Present role
EventSlot g_events[kMaxCaptures][kEventsPerCapture];

// Context-thread-only plain data (§ threading note): the spacing gate and the
// one-shot flag that stops the draw which just opened a capture from ALSO
// being emitted as an ordinary tracked-set row (OVERLAY-INVARIANTS.md risk 3).
uint64_t g_commitsSinceEnable = 0;
uint64_t g_lastOpenCommitIndex = 0;
bool g_everOpened = false;
bool g_rootJustOpened = false;

uint64_t NextOrderSerial ()
{
    return g_orderSerial.fetch_add (1, std::memory_order_relaxed) + 1;
}

bool TrackedContains (uint32_t captureIndex, uint64_t resource)
{
    if (resource == 0)
        return false;
    const TrackedSet& set = g_tracked[captureIndex];
    for (uint32_t i = 0; i < set.count; ++i) {
        if (set.resources[i] == resource)
            return true;
    }
    return false;
}

bool TryTrack (uint32_t captureIndex, uint64_t resource, uint64_t parent)
{
    TrackedSet& set = g_tracked[captureIndex];
    if (set.count >= kTrackedPerCapture)
        return false;
    set.resources[set.count] = resource;
    set.parents[set.count] = parent;
    ++set.count;
    g_captures[captureIndex].trackedCount.store (set.count, std::memory_order_relaxed);
    return true;
}

void TryCloseCapture (uint32_t captureIndex, CloseReason reason)
{
    if (captureIndex >= kMaxCaptures)
        return;
    CaptureSlot& slot = g_captures[captureIndex];
    uint32_t expected = uint32_t (SlotState::Open);
    if (!slot.state.compare_exchange_strong (expected, uint32_t (SlotState::Closed), std::memory_order_acq_rel))
        return; // someone else already closed it -- exactly-once satisfied by this CAS
    slot.closeReason.store (uint32_t (reason), std::memory_order_relaxed);
    slot.closeOrderSerial.store (NextOrderSerial (), std::memory_order_release); // CLOSE publish gate, written last
    g_capturesClosed.fetch_add (1, std::memory_order_relaxed);
    if (reason == CloseReason::Full)
        g_capturesTruncated.fetch_add (1, std::memory_order_relaxed);
    else if (reason == CloseReason::Resize)
        g_capturesAbortedByResize.fetch_add (1, std::memory_order_relaxed);
    g_activeCapture.store (uint32_t (kMaxCaptures), std::memory_order_release);
}

// Fills the shared fields, reserves a slot, and publishes. `event` must
// already carry every kind-specific payload field the caller's op sets; this
// only adds the fields common to every recorded event.
void RecordEvent (uint32_t captureIndex, CaptureSlot& slot, Event& event, Role role)
{
    event.role = role;
    event.epoch = g_epoch.load (std::memory_order_relaxed);
    event.capture = captureIndex;
    event.orderSerial = NextOrderSerial ();
    if (event.backBuffer == 0)
        event.backBuffer = g_backBuffer.load (std::memory_order_acquire);

    const uint32_t index = slot.eventsReserved.fetch_add (1, std::memory_order_relaxed);
    if (index >= kEventsPerCapture) {
        slot.eventsDropped.fetch_add (1, std::memory_order_relaxed);
        g_eventsDropped.fetch_add (1, std::memory_order_relaxed);
        TryCloseCapture (captureIndex, CloseReason::Full);
        return;
    }
    EventSlot& eventSlot = g_events[captureIndex][index];
    eventSlot.published.store (0, std::memory_order_release);
    eventSlot.epoch.store (event.epoch, std::memory_order_relaxed);
    eventSlot.capture.store (event.capture, std::memory_order_relaxed);
    eventSlot.orderSerial.store (event.orderSerial, std::memory_order_relaxed);
    eventSlot.role.store (uint32_t (event.role), std::memory_order_relaxed);
    eventSlot.kind.store (uint32_t (event.kind), std::memory_order_relaxed);
    eventSlot.drawKind.store (uint32_t (event.drawKind), std::memory_order_relaxed);
    eventSlot.drawCount.store (event.drawCount, std::memory_order_relaxed);
    eventSlot.drawsSinceRoot.store (event.drawsSinceRoot, std::memory_order_relaxed);
    eventSlot.scenePass.store (event.scenePass, std::memory_order_relaxed);
    eventSlot.rtv0.store (event.rtv0, std::memory_order_relaxed);
    eventSlot.rtvCount.store (event.rtvCount, std::memory_order_relaxed);
    eventSlot.srv0.store (event.srv0, std::memory_order_relaxed);
    eventSlot.srv1.store (event.srv1, std::memory_order_relaxed);
    eventSlot.srv2.store (event.srv2, std::memory_order_relaxed);
    eventSlot.srv3.store (event.srv3, std::memory_order_relaxed);
    eventSlot.trackedSrvMask.store (event.trackedSrvMask, std::memory_order_relaxed);
    eventSlot.trackedSrvHits.store (event.trackedSrvHits, std::memory_order_relaxed);
    eventSlot.source.store (event.source, std::memory_order_relaxed);
    eventSlot.destination.store (event.destination, std::memory_order_relaxed);
    eventSlot.resource.store (event.resource, std::memory_order_relaxed);
    eventSlot.backBuffer.store (event.backBuffer, std::memory_order_relaxed);
    eventSlot.writesBackBuffer.store (event.writesBackBuffer, std::memory_order_relaxed);
    eventSlot.readsTracked.store (event.readsTracked, std::memory_order_relaxed);
    eventSlot.readsRoot.store (event.readsRoot, std::memory_order_relaxed);
    eventSlot.trackedResource.store (event.trackedResource, std::memory_order_relaxed);
    eventSlot.trackedAdded.store (event.trackedAdded, std::memory_order_relaxed);
    eventSlot.parentResource.store (event.parentResource, std::memory_order_relaxed);
    eventSlot.succeeded.store (event.succeeded, std::memory_order_relaxed);
    eventSlot.published.store (index + 1, std::memory_order_release);
    slot.eventsRecorded.fetch_add (1, std::memory_order_relaxed);
    g_eventsRecorded.fetch_add (1, std::memory_order_relaxed);
}

// Shared prologue for every CONTEXT-thread op: resolves the active capture and
// declines (counting `eventsAfterClose`) if it raced closed underneath the
// caller. Returns nullptr when there is simply no open capture right now,
// which is the ordinary off-window state and not a decline -- matching how
// PassProvenance's own handlers do not count every call that finds nothing
// pending.
CaptureSlot* ActiveOpenCapture (uint32_t& outIndex)
{
    const uint32_t index = g_activeCapture.load (std::memory_order_acquire);
    if (index >= kMaxCaptures)
        return nullptr;
    CaptureSlot& slot = g_captures[index];
    if (slot.state.load (std::memory_order_acquire) != uint32_t (SlotState::Open)) {
        g_eventsAfterClose.fetch_add (1, std::memory_order_relaxed);
        return nullptr;
    }
    outIndex = index;
    return &slot;
}

} // namespace

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void Reset ()
{
    g_epoch.fetch_add (1, std::memory_order_acq_rel);
    g_orderSerial.store (0, std::memory_order_relaxed);
    g_activeCapture.store (uint32_t (kMaxCaptures), std::memory_order_relaxed);
    g_backBuffer.store (0, std::memory_order_relaxed);
    g_capturesOpened.store (0, std::memory_order_relaxed);
    g_capturesClosed.store (0, std::memory_order_relaxed);
    g_capturesTruncated.store (0, std::memory_order_relaxed);
    g_capturesAbortedByResize.store (0, std::memory_order_relaxed);
    g_rootsSeen.store (0, std::memory_order_relaxed);
    g_eventsRecorded.store (0, std::memory_order_relaxed);
    g_eventsDropped.store (0, std::memory_order_relaxed);
    g_eventsAfterClose.store (0, std::memory_order_relaxed);
    g_unmodelledWork.store (0, std::memory_order_relaxed);
    g_trackedOverflowTotal.store (0, std::memory_order_relaxed);
    g_commitsSinceEnable = 0;
    g_lastOpenCommitIndex = 0;
    g_everOpened = false;
    g_rootJustOpened = false;
    for (size_t i = 0; i < kMaxCaptures; ++i) {
        CaptureSlot& slot = g_captures[i];
        slot.state.store (uint32_t (SlotState::Empty), std::memory_order_relaxed);
        slot.rootScenePass = 0;
        slot.rootResource = 0;
        slot.rootEventSerial = 0;
        slot.openOrderSerial.store (0, std::memory_order_relaxed);
        slot.closeOrderSerial.store (0, std::memory_order_relaxed);
        slot.closeReason.store (uint32_t (CloseReason::Open), std::memory_order_relaxed);
        slot.presentsSeen.store (0, std::memory_order_relaxed);
        slot.drawsSinceRoot.store (0, std::memory_order_relaxed);
        slot.rootWrites.store (0, std::memory_order_relaxed);
        slot.unmodelledWork.store (0, std::memory_order_relaxed);
        slot.trackedCount.store (0, std::memory_order_relaxed);
        slot.trackedOverflow.store (0, std::memory_order_relaxed);
        slot.eventsReserved.store (0, std::memory_order_relaxed);
        slot.eventsRecorded.store (0, std::memory_order_relaxed);
        slot.eventsDropped.store (0, std::memory_order_relaxed);
        g_tracked[i] = TrackedSet {};
        for (size_t e = 0; e < kEventsPerCapture; ++e)
            g_events[i][e].published.store (0, std::memory_order_relaxed);
    }
}

void OnImageCommitted (uint64_t scenePass, uint64_t modelGeneration, uint64_t sceneColorResource,
                       uint64_t rootEventSerial)
{
    (void) modelGeneration; // accepted for symmetry with the source call site; not a stored field this stage
    if (!Enabled () || scenePass == 0 || sceneColorResource == 0)
        return;
    g_rootsSeen.fetch_add (1, std::memory_order_relaxed);

    const uint32_t active = g_activeCapture.load (std::memory_order_acquire);
    if (active < kMaxCaptures) {
        CaptureSlot& slot = g_captures[active];
        if (slot.state.load (std::memory_order_acquire) != uint32_t (SlotState::Open)) {
            g_eventsAfterClose.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        Event event;
        event.kind = Kind::NextRoot;
        event.scenePass = scenePass;
        event.resource = sceneColorResource;
        event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
        RecordEvent (active, slot, event, Role::Context);
        return; // an open capture never opens a second one
    }

    ++g_commitsSinceEnable;
    const bool spacingAllows =
        !g_everOpened || (g_commitsSinceEnable - g_lastOpenCommitIndex) >= uint64_t (kCaptureSpacing);
    const uint32_t opened = g_capturesOpened.load (std::memory_order_relaxed);
    if (!spacingAllows || opened >= kMaxCaptures)
        return; // not yet due -- the ordinary off-window state, not a decline

    const uint32_t index = g_capturesOpened.fetch_add (1, std::memory_order_acq_rel);
    if (index >= kMaxCaptures)
        return;
    CaptureSlot& slot = g_captures[index];
    slot.closeReason.store (uint32_t (CloseReason::Open), std::memory_order_relaxed);
    g_tracked[index] = TrackedSet {};
    g_tracked[index].resources[0] = sceneColorResource;
    g_tracked[index].count = 1;
    slot.trackedCount.store (1, std::memory_order_relaxed);
    slot.rootScenePass = scenePass;
    slot.rootResource = sceneColorResource;
    slot.rootEventSerial = rootEventSerial;
    slot.openOrderSerial.store (NextOrderSerial (), std::memory_order_release); // OPEN publish gate, written last
    slot.state.store (uint32_t (SlotState::Open), std::memory_order_release);
    g_activeCapture.store (index, std::memory_order_release);
    g_rootJustOpened = true;
    g_everOpened = true;
    g_lastOpenCommitIndex = g_commitsSinceEnable;

    Event event;
    event.kind = Kind::Root;
    event.scenePass = scenePass;
    event.resource = sceneColorResource;
    event.trackedResource = sceneColorResource;
    event.trackedAdded = 1;
    RecordEvent (index, slot, event, Role::Context);
}

void OnDraw (passprovenance::DrawKind drawKind, uint32_t drawCount, const uint64_t* rtvs, size_t rtvSlots,
             const uint64_t* srvs, size_t srvSlots)
{
    if (!Enabled ())
        return;
    uint32_t captureIndex = 0;
    CaptureSlot* slotPtr = ActiveOpenCapture (captureIndex);
    if (slotPtr == nullptr)
        return;
    CaptureSlot& slot = *slotPtr;

    if (g_rootJustOpened) {
        g_rootJustOpened = false;
        return; // this draw IS the Root event already recorded by OnImageCommitted
    }
    const uint64_t drawsSinceRoot = slot.drawsSinceRoot.fetch_add (1, std::memory_order_relaxed) + 1;
    const uint64_t backBuffer = g_backBuffer.load (std::memory_order_acquire);
    const uint64_t rootResource = slot.rootResource;

    uint32_t trackedSrvMask = 0;
    uint32_t trackedSrvHits = 0;
    bool readsRoot = false;
    uint64_t firstTrackedSrv = 0;
    for (size_t i = 0; i < srvSlots; ++i) {
        const uint64_t resource = srvs != nullptr ? srvs[i] : 0;
        if (resource == 0 || !TrackedContains (captureIndex, resource))
            continue;
        ++trackedSrvHits;
        if (i < 32)
            trackedSrvMask |= (uint32_t (1) << i);
        if (firstTrackedSrv == 0)
            firstTrackedSrv = resource;
        if (resource == rootResource)
            readsRoot = true;
    }
    const bool readsTracked = trackedSrvHits != 0;

    uint32_t rtvCount = 0;
    bool writesBackBuffer = false;
    bool writesTrackedNonRoot = false;
    bool writesOnlyRoot = true;
    for (size_t i = 0; i < rtvSlots; ++i) {
        const uint64_t target = rtvs != nullptr ? rtvs[i] : 0;
        if (target == 0)
            continue;
        ++rtvCount;
        if (target != rootResource)
            writesOnlyRoot = false;
        if (target == backBuffer)
            writesBackBuffer = true;
        else if (target != rootResource && TrackedContains (captureIndex, target))
            writesTrackedNonRoot = true;
    }
    if (rtvCount == 0)
        writesOnlyRoot = false;

    if (!readsTracked && !writesBackBuffer && !writesTrackedNonRoot) {
        if (writesOnlyRoot)
            slot.rootWrites.fetch_add (1, std::memory_order_relaxed);
        return; // no candidate relation to S/B in this draw; not further counted (spec table)
    }

    uint64_t trackedResourceAdded = 0;
    uint32_t trackedAdded = 0;
    if (readsTracked) {
        for (size_t i = 0; i < rtvSlots; ++i) {
            const uint64_t target = rtvs != nullptr ? rtvs[i] : 0;
            if (target == 0 || target == backBuffer || TrackedContains (captureIndex, target))
                continue;
            if (TryTrack (captureIndex, target, firstTrackedSrv)) {
                if (trackedResourceAdded == 0)
                    trackedResourceAdded = target;
                ++trackedAdded;
            }
            else {
                slot.trackedOverflow.fetch_add (1, std::memory_order_relaxed);
                g_trackedOverflowTotal.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    Event event;
    event.kind = Kind::Draw;
    event.drawKind = drawKind;
    event.drawCount = drawCount;
    event.drawsSinceRoot = drawsSinceRoot;
    event.rtv0 = rtvSlots > 0 && rtvs != nullptr ? rtvs[0] : 0;
    event.rtvCount = rtvCount;
    event.srv0 = srvSlots > 0 && srvs != nullptr ? srvs[0] : 0;
    event.srv1 = srvSlots > 1 && srvs != nullptr ? srvs[1] : 0;
    event.srv2 = srvSlots > 2 && srvs != nullptr ? srvs[2] : 0;
    event.srv3 = srvSlots > 3 && srvs != nullptr ? srvs[3] : 0;
    event.trackedSrvMask = trackedSrvMask;
    event.trackedSrvHits = trackedSrvHits;
    event.backBuffer = backBuffer;
    event.writesBackBuffer = writesBackBuffer;
    event.readsTracked = readsTracked;
    event.readsRoot = readsRoot;
    event.trackedResource = trackedResourceAdded;
    event.trackedAdded = trackedAdded;
    event.parentResource = firstTrackedSrv;
    RecordEvent (captureIndex, slot, event, Role::Context);
}

namespace {

// Copy and partial copy share one rule and differ only in `kind`. A copy out
// of a tracked resource tracks its destination and records the link, so an
// S -> copy -> T -> draw -> B chain can be walked back from B's writer.
void RecordCopy (Kind kind, uint64_t destination, uint64_t source)
{
    if (!Enabled ())
        return;
    uint32_t captureIndex = 0;
    CaptureSlot* slotPtr = ActiveOpenCapture (captureIndex);
    if (slotPtr == nullptr)
        return;
    CaptureSlot& slot = *slotPtr;
    const uint64_t backBuffer = g_backBuffer.load (std::memory_order_acquire);
    const bool srcTracked = TrackedContains (captureIndex, source);
    const bool dstIsBackBuffer = destination != 0 && destination == backBuffer;
    const bool dstTracked = TrackedContains (captureIndex, destination);
    if (!srcTracked && !dstIsBackBuffer && !dstTracked)
        return;
    Event event;
    event.kind = kind;
    event.source = source;
    event.destination = destination;
    event.writesBackBuffer = dstIsBackBuffer;
    event.readsTracked = srcTracked;
    event.readsRoot = source != 0 && source == slot.rootResource;
    if (srcTracked) {
        event.parentResource = source;
        if (destination != 0 && !dstIsBackBuffer && !dstTracked) {
            if (TryTrack (captureIndex, destination, source)) {
                event.trackedResource = destination;
                event.trackedAdded = 1;
            }
            else {
                slot.trackedOverflow.fetch_add (1, std::memory_order_relaxed);
                g_trackedOverflowTotal.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }
    event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
    RecordEvent (captureIndex, slot, event, Role::Context);
}

} // namespace

void OnCopy (uint64_t destination, uint64_t source)
{
    RecordCopy (Kind::Copy, destination, source);
}

void OnPartialCopy (uint64_t destination, uint64_t source)
{
    RecordCopy (Kind::PartialCopy, destination, source);
}

void OnClear (uint64_t resource)
{
    if (!Enabled ())
        return;
    uint32_t captureIndex = 0;
    CaptureSlot* slotPtr = ActiveOpenCapture (captureIndex);
    if (slotPtr == nullptr)
        return;
    CaptureSlot& slot = *slotPtr;
    const uint64_t backBuffer = g_backBuffer.load (std::memory_order_acquire);
    const bool relevant = (resource != 0 && resource == backBuffer) || TrackedContains (captureIndex, resource);
    if (!relevant)
        return;
    Event event;
    event.kind = Kind::Clear;
    event.resource = resource;
    event.writesBackBuffer = resource != 0 && resource == backBuffer;
    event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
    RecordEvent (captureIndex, slot, event, Role::Context);
}

void OnResourceWrite (uint64_t resource)
{
    if (!Enabled ())
        return;
    uint32_t captureIndex = 0;
    CaptureSlot* slotPtr = ActiveOpenCapture (captureIndex);
    if (slotPtr == nullptr)
        return;
    CaptureSlot& slot = *slotPtr;
    const uint64_t backBuffer = g_backBuffer.load (std::memory_order_acquire);
    const bool relevant = (resource != 0 && resource == backBuffer) || TrackedContains (captureIndex, resource);
    if (!relevant)
        return;
    Event event;
    event.kind = Kind::ResourceWrite;
    event.resource = resource;
    event.writesBackBuffer = resource != 0 && resource == backBuffer;
    event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
    RecordEvent (captureIndex, slot, event, Role::Context);
}

void OnUnmodelledWork ()
{
    if (!Enabled ())
        return;
    uint32_t captureIndex = 0;
    CaptureSlot* slotPtr = ActiveOpenCapture (captureIndex);
    if (slotPtr == nullptr)
        return;
    CaptureSlot& slot = *slotPtr;
    slot.unmodelledWork.fetch_add (1, std::memory_order_relaxed);
    g_unmodelledWork.fetch_add (1, std::memory_order_relaxed);
    Event event;
    event.kind = Kind::UnmodelledWork;
    event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
    RecordEvent (captureIndex, slot, event, Role::Context); // always recorded while open (spec table)
}

bool BeginPresent (IDXGISwapChain* swapChain)
{
    if (!Enabled () || swapChain == nullptr)
        return false;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT result = swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &texture);
    const uint64_t backBuffer =
        (SUCCEEDED (result) && texture != nullptr) ? uint64_t (uintptr_t (static_cast<ID3D11Resource*> (texture))) : 0;
    if (texture != nullptr)
        texture->Release ();
    // Published on EVERY nominated Present while enabled, not only while a
    // capture is open -- OVERLAY-INVARIANTS.md risk 4 (flip-model identity is
    // not stable across a capture's own Presents; never cache it).
    g_backBuffer.store (backBuffer, std::memory_order_release);

    const uint32_t captureIndex = g_activeCapture.load (std::memory_order_acquire);
    if (captureIndex < kMaxCaptures) {
        CaptureSlot& slot = g_captures[captureIndex];
        if (slot.state.load (std::memory_order_acquire) == uint32_t (SlotState::Open)) {
            Event event;
            event.kind = Kind::PresentBegin;
            event.backBuffer = backBuffer;
            event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
            RecordEvent (captureIndex, slot, event, Role::Present);
        }
    }
    return true;
}

void EndPresent (bool succeeded)
{
    const uint32_t captureIndex = g_activeCapture.load (std::memory_order_acquire);
    if (captureIndex >= kMaxCaptures)
        return;
    CaptureSlot& slot = g_captures[captureIndex];
    if (slot.state.load (std::memory_order_acquire) != uint32_t (SlotState::Open)) {
        g_eventsAfterClose.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    Event event;
    event.kind = Kind::PresentEnd;
    event.succeeded = succeeded;
    event.backBuffer = g_backBuffer.load (std::memory_order_acquire);
    event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
    RecordEvent (captureIndex, slot, event, Role::Present);
    if (succeeded) {
        const uint32_t seen = slot.presentsSeen.fetch_add (1, std::memory_order_relaxed) + 1;
        if (seen >= kPresentsPerCapture)
            TryCloseCapture (captureIndex, CloseReason::Presents);
    }
}

void OnResizeBuffers ()
{
    if (!Enabled ())
        return;
    const uint32_t captureIndex = g_activeCapture.load (std::memory_order_acquire);
    if (captureIndex < kMaxCaptures)
        TryCloseCapture (captureIndex, CloseReason::Resize);
    g_backBuffer.store (0, std::memory_order_release);
}

size_t CopyEvents (Event* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    const uint32_t opened = g_capturesOpened.load (std::memory_order_acquire);
    const uint32_t total = opened < kMaxCaptures ? opened : uint32_t (kMaxCaptures);
    size_t copied = 0;
    for (uint32_t c = 0; c < total && copied < capacity; ++c) {
        const uint32_t reserved = g_captures[c].eventsReserved.load (std::memory_order_acquire);
        const uint32_t limit = reserved < kEventsPerCapture ? reserved : uint32_t (kEventsPerCapture);
        for (uint32_t i = 0; i < limit && copied < capacity; ++i) {
            const EventSlot& slot = g_events[c][i];
            const uint32_t expected = i + 1;
            if (slot.published.load (std::memory_order_acquire) != expected)
                continue;
            Event event;
            event.epoch = slot.epoch.load (std::memory_order_relaxed);
            event.capture = slot.capture.load (std::memory_order_relaxed);
            event.orderSerial = slot.orderSerial.load (std::memory_order_relaxed);
            event.role = Role (slot.role.load (std::memory_order_relaxed));
            event.kind = Kind (slot.kind.load (std::memory_order_relaxed));
            event.drawKind = passprovenance::DrawKind (slot.drawKind.load (std::memory_order_relaxed));
            event.drawCount = slot.drawCount.load (std::memory_order_relaxed);
            event.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
            event.scenePass = slot.scenePass.load (std::memory_order_relaxed);
            event.rtv0 = slot.rtv0.load (std::memory_order_relaxed);
            event.rtvCount = slot.rtvCount.load (std::memory_order_relaxed);
            event.srv0 = slot.srv0.load (std::memory_order_relaxed);
            event.srv1 = slot.srv1.load (std::memory_order_relaxed);
            event.srv2 = slot.srv2.load (std::memory_order_relaxed);
            event.srv3 = slot.srv3.load (std::memory_order_relaxed);
            event.trackedSrvMask = slot.trackedSrvMask.load (std::memory_order_relaxed);
            event.trackedSrvHits = slot.trackedSrvHits.load (std::memory_order_relaxed);
            event.source = slot.source.load (std::memory_order_relaxed);
            event.destination = slot.destination.load (std::memory_order_relaxed);
            event.resource = slot.resource.load (std::memory_order_relaxed);
            event.backBuffer = slot.backBuffer.load (std::memory_order_relaxed);
            event.writesBackBuffer = slot.writesBackBuffer.load (std::memory_order_relaxed);
            event.readsTracked = slot.readsTracked.load (std::memory_order_relaxed);
            event.readsRoot = slot.readsRoot.load (std::memory_order_relaxed);
            event.trackedResource = slot.trackedResource.load (std::memory_order_relaxed);
            event.trackedAdded = slot.trackedAdded.load (std::memory_order_relaxed);
            event.parentResource = slot.parentResource.load (std::memory_order_relaxed);
            event.succeeded = slot.succeeded.load (std::memory_order_relaxed);
            if (slot.published.load (std::memory_order_acquire) == expected)
                out[copied++] = event;
        }
    }
    return copied;
}

size_t CopyCaptures (Capture* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    const uint32_t opened = g_capturesOpened.load (std::memory_order_acquire);
    const uint32_t total = opened < kMaxCaptures ? opened : uint32_t (kMaxCaptures);
    size_t copied = 0;
    for (uint32_t i = 0; i < total && copied < capacity; ++i) {
        const CaptureSlot& slot = g_captures[i];
        const uint64_t openSerial = slot.openOrderSerial.load (std::memory_order_acquire);
        if (openSerial == 0)
            continue; // reserved but not yet published; will appear on a later poll
        Capture capture;
        capture.capture = i;
        capture.rootPass = slot.rootScenePass;
        capture.rootResource = slot.rootResource;
        capture.rootEventSerial = slot.rootEventSerial;
        capture.openOrderSerial = openSerial;
        capture.closeOrderSerial = slot.closeOrderSerial.load (std::memory_order_acquire);
        capture.closeReason = CloseReason (slot.closeReason.load (std::memory_order_relaxed));
        capture.presentsSeen = slot.presentsSeen.load (std::memory_order_relaxed);
        capture.drawsSinceRoot = slot.drawsSinceRoot.load (std::memory_order_relaxed);
        capture.rootWrites = slot.rootWrites.load (std::memory_order_relaxed);
        capture.unmodelledWork = slot.unmodelledWork.load (std::memory_order_relaxed);
        capture.trackedCount = slot.trackedCount.load (std::memory_order_relaxed);
        capture.trackedOverflow = slot.trackedOverflow.load (std::memory_order_relaxed);
        capture.eventsRecorded = slot.eventsRecorded.load (std::memory_order_relaxed);
        capture.eventsDropped = slot.eventsDropped.load (std::memory_order_relaxed);
        out[copied++] = capture;
    }
    return copied;
}

Stats GetStats ()
{
    Stats stats;
    stats.enabled = Enabled ();
    stats.epoch = g_epoch.load (std::memory_order_relaxed);
    const uint32_t opened = g_capturesOpened.load (std::memory_order_relaxed);
    stats.capturesOpened = opened < kMaxCaptures ? opened : uint32_t (kMaxCaptures);
    stats.capturesClosed = g_capturesClosed.load (std::memory_order_relaxed);
    stats.capturesTruncated = g_capturesTruncated.load (std::memory_order_relaxed);
    stats.capturesAbortedByResize = g_capturesAbortedByResize.load (std::memory_order_relaxed);
    stats.rootsSeen = g_rootsSeen.load (std::memory_order_relaxed);
    stats.eventsRecorded = g_eventsRecorded.load (std::memory_order_relaxed);
    stats.eventsDropped = g_eventsDropped.load (std::memory_order_relaxed);
    stats.eventsAfterClose = g_eventsAfterClose.load (std::memory_order_relaxed);
    stats.unmodelledWork = g_unmodelledWork.load (std::memory_order_relaxed);
    stats.trackedOverflow = g_trackedOverflowTotal.load (std::memory_order_relaxed);
    stats.lastBackBuffer = g_backBuffer.load (std::memory_order_acquire);
    return stats;
}

} // namespace imagetransfer
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
