// ArchViz/Dxgi/ConstantBufferCapture -- see the header. Extracted from
// ViewMatrixCandidates.cpp (2026-09-13) when the pair and half-test scorers
// pushed that file past the size cap; the seam was already the call these two
// halves share.

// windows.h defines min/max as macros, which makes every std::min below a
// syntax error rather than an overload problem.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ArchViz/Dxgi/ConstantBufferCapture.hpp"

#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace viewmatrix {

namespace {

// The present detour's frame counter, mirrored here so the detours can stamp
// what they capture without reaching into the present hook.
std::atomic<uint64_t> g_frameId {0};

std::atomic<uint64_t> g_mapsSeen {0};
std::atomic<uint64_t> g_mapsNotConstantBuffer {0};
std::atomic<uint64_t> g_mapsTooLarge {0};
std::atomic<uint64_t> g_mapsNoSlot {0};
std::atomic<uint32_t> g_largestConstantBytes {0};

// ---- sizing a constant buffer ----------------------------------------------
// Moved here from `ContextHook` (2026-09-13) when that file reached the size
// cap, and it belongs here on the merits: what counts as a capturable buffer is
// a question about finding a view matrix. See the header for the caching rule.
constexpr size_t kBufferCacheSize = 256;
struct BufferInfo {
    std::atomic<uint64_t> resource {0};
    std::atomic<uint32_t> byteWidth {0};
    std::atomic<uint32_t> isConstantBuffer {0};
};
BufferInfo g_bufferCache[kBufferCacheSize];
std::atomic<bool> g_bufferCacheFull {false};

// ---- what is mapped right now ----------------------------------------------
constexpr size_t kMapTableSize = 16;
struct MapEntry {
    std::atomic<uint64_t> resource {0};
    std::atomic<uint64_t> pointer {0};
    std::atomic<uint32_t> bytes {0};      // how much of it to copy at Unmap
    std::atomic<uint32_t> bufferBytes {0};// the whole buffer, for clamping a window
};
MapEntry g_mapped[kMapTableSize];

// ---- windows of a ring that have been bound and not yet captured -----------
// ⚠️ SMALL ON PURPOSE. Every entry here is a kilobyte of write-combined memory
// that some later Unmap will read, and reads from write-combined memory are the
// most expensive thing this whole discovery path does -- run ten measured the
// constant-buffer slots at roughly a tenth of Archicad's frame rate while
// copying NOTHING. Four windows per drain is enough to catch a per-view buffer
// among the per-object ones; more would buy noise at Archicad's expense.
constexpr size_t kPendingWindows = 8;
constexpr size_t kWindowsPerDrain = 4;
struct PendingWindow {
    std::atomic<uint64_t> buffer {0};
    std::atomic<uint32_t> byteOffset {0};
    std::atomic<uint32_t> slotKind {0};
    std::atomic<uint32_t> bindSlot {0};
    // Which frame the BIND happened in. The capture that drains it may be a
    // draw later and, at a frame boundary, a frame later -- which is the one
    // case stage 4 has to be able to see.
    std::atomic<uint64_t> frameId {0};
};
PendingWindow g_pending[kPendingWindows];
std::atomic<uint32_t> g_pendingCursor {0};
std::atomic<uint64_t> g_windowsCaptured {0};
std::atomic<uint64_t> g_windowsDropped {0};
std::atomic<uint32_t> g_largestBoundOffset {0};

}   // namespace

uint32_t ConstantBufferWidth (ID3D11Resource* resource)
{
    const uint64_t key = uint64_t (uintptr_t (resource));
    if (key == 0)
        return 0;

    for (BufferInfo& entry : g_bufferCache) {
        const uint64_t seen = entry.resource.load (std::memory_order_acquire);
        if (seen == key) {
            return entry.isConstantBuffer.load (std::memory_order_relaxed)
                ? entry.byteWidth.load (std::memory_order_relaxed) : 0;
        }
        if (seen != 0)
            continue;

        uint32_t width = 0;
        uint32_t isConstant = 0;
        ID3D11Buffer* buffer = nullptr;
        if (SUCCEEDED (resource->QueryInterface (__uuidof (ID3D11Buffer), (void**) &buffer)) &&
            buffer != nullptr) {
            D3D11_BUFFER_DESC desc = {};
            buffer->GetDesc (&desc);
            if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) {
                isConstant = 1;
                width = desc.ByteWidth;
            }
            buffer->Release ();
        }
        // ⚠️ THE PAYLOAD IS PUBLISHED BEFORE THE KEY, for the reason
        // `RememberChainWindow` gives: a reader that saw the key first could
        // read a zero width and cache "this buffer is empty" for the session.
        entry.byteWidth.store (width, std::memory_order_relaxed);
        entry.isConstantBuffer.store (isConstant, std::memory_order_relaxed);
        entry.resource.store (key, std::memory_order_release);
        return isConstant ? width : 0;
    }

    // ⚠️ WHEN THE CACHE IS FULL IT STOPS ASKING RATHER THAN EVICTING. An eviction
    // policy on a lock-free table read from a render thread is a source of races
    // for no benefit: a scene with more distinct buffers than this has bigger
    // problems than a few uncached ones.
    g_bufferCacheFull.store (true, std::memory_order_relaxed);
    return 0;
}

uint32_t OnMapped (ID3D11Resource* resource, const void* mappedPointer)
{
    if (resource == nullptr || mappedPointer == nullptr)
        return 0;

    // ⚠️ COUNTED BEFORE ANYTHING ELSE, AND EVERY REJECTION IS NAMED. All four
    // of these are relaxed adds on already-hot cache lines, which is a price
    // worth paying once: a run that captures nothing has to be able to say
    // WHICH of "Archicad does not Map its constant buffers", "its constant
    // buffers are bigger than we are willing to copy" and "we ran out of table"
    // is true, and without these the report can only shrug.
    g_mapsSeen.fetch_add (1, std::memory_order_relaxed);

    const uint32_t width = ConstantBufferWidth (resource);
    if (width == 0) {
        g_mapsNotConstantBuffer.fetch_add (1, std::memory_order_relaxed);
        return 0;
    }

    // ⚠️ THE WIDEST ONE IS REMEMBERED EVEN THOUGH IT IS REFUSED, because the
    // number decides the next move. A few kilobytes means raise the cap; a
    // megabyte means Archicad packs its constants into one big ring and the
    // capture has to follow `firstConstant` into it instead of copying from the
    // start -- two different pieces of work, and this is what tells them apart.
    uint32_t seen = g_largestConstantBytes.load (std::memory_order_relaxed);
    while (width > seen &&
           !g_largestConstantBytes.compare_exchange_weak (seen, width,
                   std::memory_order_relaxed))
        ;

    // ⚠️ A BUFFER WIDER THAN THE WINDOW IS NO LONGER REFUSED, it is followed.
    // `mapsTooLarge` is kept and still counted, because on a host that does NOT
    // ring its constants the number is still the thing worth knowing -- but it
    // is now a description of the traffic, not a rejection.
    const uint32_t copyBytes = std::min (width, kMaxTrackedBytes);
    if (width > kMaxTrackedBytes)
        g_mapsTooLarge.fetch_add (1, std::memory_order_relaxed);

    const uint64_t key = uint64_t (uintptr_t (resource));
    for (MapEntry& entry : g_mapped) {
        uint64_t expected = 0;
        // Claimed by CAS rather than by a store. The immediate context is single
        // threaded by D3D11's own contract, but Archicad may drive a deferred one
        // on another thread through the same vtable, and a torn claim here would
        // copy one buffer's bytes under another buffer's name.
        if (entry.resource.compare_exchange_strong (expected, key, std::memory_order_acq_rel)) {
            entry.pointer.store (uint64_t (uintptr_t (mappedPointer)), std::memory_order_relaxed);
            entry.bufferBytes.store (width, std::memory_order_relaxed);
            entry.bytes.store (copyBytes, std::memory_order_release);
            return copyBytes;
        }
    }
    // ⚠️ TABLE FULL. This used to return silently, on the reasoning that sixteen
    // simultaneous constant-buffer maps would mean D3D11's own threading
    // contract had been broken. That reasoning is sound and the silence was
    // still wrong: it made one of the three ways to capture nothing invisible,
    // and this rung has already lost six runs to an instrument that failed
    // quietly. A non-zero count here is a real finding, not a tuning knob.
    g_mapsNoSlot.fetch_add (1, std::memory_order_relaxed);
    return 0;
}

uint32_t OnUnmapping (ID3D11Resource* resource)
{
    const uint64_t key = uint64_t (uintptr_t (resource));
    if (key == 0)
        return 0;
    for (MapEntry& entry : g_mapped) {
        if (entry.resource.load (std::memory_order_acquire) != key)
            continue;
        const void* pointer =
            (const void*) (uintptr_t) entry.pointer.load (std::memory_order_relaxed);
        const uint32_t bytes = entry.bytes.load (std::memory_order_acquire);
        const uint32_t bufferBytes = entry.bufferBytes.load (std::memory_order_relaxed);
        entry.pointer.store (0, std::memory_order_relaxed);
        entry.bytes.store (0, std::memory_order_relaxed);
        entry.bufferBytes.store (0, std::memory_order_relaxed);
        entry.resource.store (0, std::memory_order_release);
        if (pointer == nullptr || bytes == 0)
            return 0;

        // A buffer no wider than the window is its own window: copy from zero,
        // exactly as before there were rings.
        if (bufferBytes <= kMaxTrackedBytes) {
            OnConstantBufferWrite (resource, 0, pointer, bytes, 0);
            return bytes;
        }

        // ⚠️ A RING. Copy the windows something has bound, and nothing else --
        // the buffer is megabytes and all but a few kilobytes of it is other
        // draws' data. See the header for why these windows are one draw stale.
        const unsigned char* base = static_cast<const unsigned char*> (pointer);
        uint32_t drained = 0;
        for (PendingWindow& window : g_pending) {
            if (drained >= kWindowsPerDrain)
                break;
            if (window.buffer.load (std::memory_order_acquire) != key)
                continue;
            const uint32_t offset = window.byteOffset.load (std::memory_order_relaxed);
            // ⚠️ CLAMPED AGAINST THE BUFFER, NOT THE WINDOW. `firstConstant`
            // comes from Archicad and a copy past the end of an eight-megabyte
            // mapped range is a wild read on the render thread.
            if (offset >= bufferBytes)
                continue;
            const uint32_t span = std::min (kMaxTrackedBytes, bufferBytes - offset);
            if (span < 64)
                continue;
            OnConstantBufferWrite (resource, 0, base + offset, span, offset);
            // ⚠️ THE BIND'S FRAME, NOT THE UNMAP'S. What these bytes describe is
            // the draw that BOUND this window; reading them a draw later does
            // not make them newer. Stamping with the bind's frame is what lets a
            // consumer refuse a pair that straddles a Present.
            LabelCapturedWindow (resource, offset,
                    window.slotKind.load (std::memory_order_relaxed),
                    window.bindSlot.load (std::memory_order_relaxed),
                    window.frameId.load (std::memory_order_relaxed));
            window.buffer.store (0, std::memory_order_release);
            g_windowsCaptured.fetch_add (1, std::memory_order_relaxed);
            ++drained;
        }
        return drained * kMaxTrackedBytes;
    }
    return 0;
}

void OnConstantBufferBound (uint32_t slotKind, uint32_t startSlot, ID3D11Buffer* buffer)
{
    if (buffer == nullptr)
        return;
    // ⚠️ IT DOES NOT CLAIM A SLOT. Archicad binds far more buffers than it
    // updates, and letting a bind fill the table would push the handful that
    // actually carry per-frame data out of it. Only a captured WRITE claims an
    // entry; a bind merely labels one that already exists.
    LabelCapturedWindow (reinterpret_cast<ID3D11Resource*> (buffer), 0, slotKind, startSlot,
            renderstate::CurrentScenePass ().generation);
}

void OnConstantBufferBoundWindow (uint32_t slotKind, uint32_t startSlot, ID3D11Buffer* buffer,
                                  uint32_t byteOffset)
{
    if (buffer == nullptr)
        return;
    if (byteOffset == 0) {
        // Offset zero is an ordinary whole-buffer bind; nothing to remember.
        OnConstantBufferBound (slotKind, startSlot, buffer);
        return;
    }

    const uint64_t key = uint64_t (uintptr_t (buffer));
    uint32_t largest = g_largestBoundOffset.load (std::memory_order_relaxed);
    while (byteOffset > largest &&
           !g_largestBoundOffset.compare_exchange_weak (largest, byteOffset,
                   std::memory_order_relaxed))
        ;

    // Already pending? Binding the same window twice before it is drained is
    // ordinary -- several draws share one set of constants.
    for (PendingWindow& window : g_pending) {
        if (window.buffer.load (std::memory_order_acquire) == key &&
            window.byteOffset.load (std::memory_order_relaxed) == byteOffset)
            return;
    }

    // ⚠️ OLDEST-WINS EVICTION, VIA A CURSOR, AND THE LOSS IS COUNTED. A bind
    // arrives thousands of times a frame and an Unmap drains at most four
    // windows, so this table is meant to overflow; what must not happen is for
    // the overflow to be silent. A free slot is preferred; a round-robin
    // overwrite is the fallback.
    for (PendingWindow& window : g_pending) {
        uint64_t expected = 0;
        if (window.buffer.compare_exchange_strong (expected, key, std::memory_order_acq_rel)) {
            window.byteOffset.store (byteOffset, std::memory_order_relaxed);
            window.slotKind.store (slotKind, std::memory_order_relaxed);
            window.bindSlot.store (startSlot, std::memory_order_relaxed);
            window.frameId.store (renderstate::CurrentScenePass ().generation,
                    std::memory_order_relaxed);
            return;
        }
    }

    const uint32_t at = g_pendingCursor.fetch_add (1, std::memory_order_relaxed) % kPendingWindows;
    PendingWindow& window = g_pending[at];
    window.byteOffset.store (byteOffset, std::memory_order_relaxed);
    window.slotKind.store (slotKind, std::memory_order_relaxed);
    window.bindSlot.store (startSlot, std::memory_order_relaxed);
    window.frameId.store (renderstate::CurrentScenePass ().generation,
            std::memory_order_relaxed);
    window.buffer.store (key, std::memory_order_release);
    g_windowsDropped.fetch_add (1, std::memory_order_relaxed);
}

void BeginFrame (uint64_t frameId)
{
    g_frameId.store (frameId, std::memory_order_relaxed);
}

uint64_t CurrentCaptureFrame ()
{
    return g_frameId.load (std::memory_order_relaxed);
}

// ⚠️ THE STAMP IS THE SCENE PASS, NOT THE PRESENT INTERVAL, and run seventeen is
// why this is worth saying twice. Pairing on the Present frame id asked the main
// thread to be in the same frame as the render thread, which it can never be:
// 2720 attempts resolved 0 pairs, every one refused because the render thread
// had already advanced the counter by the time the tick read it. A scene-pass
// generation is a property of what Archicad drew, not of when we looked, so it
// is comparable from any thread at any time -- and it also refuses the pairing
// a Present id would wrongly allow, between a shadow pass and the main pass in
// the same interval.
uint64_t CurrentScenePassGeneration ()
{
    return renderstate::CurrentScenePass ().generation;
}

void FillCaptureStats (CandidateStats& stats)
{
    stats.mapsSeen = g_mapsSeen.load (std::memory_order_relaxed);
    stats.mapsNotConstantBuffer = g_mapsNotConstantBuffer.load (std::memory_order_relaxed);
    stats.mapsTooLarge = g_mapsTooLarge.load (std::memory_order_relaxed);
    stats.mapsNoSlot = g_mapsNoSlot.load (std::memory_order_relaxed);
    stats.largestConstantBytes = g_largestConstantBytes.load (std::memory_order_relaxed);
    stats.bufferCacheFull = g_bufferCacheFull.load (std::memory_order_relaxed);
    stats.windowsCaptured = g_windowsCaptured.load (std::memory_order_relaxed);
    stats.windowsDropped = g_windowsDropped.load (std::memory_order_relaxed);
    stats.largestBoundOffset = g_largestBoundOffset.load (std::memory_order_relaxed);
}

void ResetCapture ()
{
    for (BufferInfo& entry : g_bufferCache) {
        entry.resource.store (0, std::memory_order_relaxed);
        entry.byteWidth.store (0, std::memory_order_relaxed);
        entry.isConstantBuffer.store (0, std::memory_order_relaxed);
    }
    g_bufferCacheFull.store (false, std::memory_order_relaxed);
    for (MapEntry& entry : g_mapped) {
        entry.resource.store (0, std::memory_order_relaxed);
        entry.pointer.store (0, std::memory_order_relaxed);
        entry.bytes.store (0, std::memory_order_relaxed);
        entry.bufferBytes.store (0, std::memory_order_relaxed);
    }
    for (PendingWindow& window : g_pending)
        window.buffer.store (0, std::memory_order_relaxed);
    g_pendingCursor.store (0, std::memory_order_relaxed);
    g_mapsSeen.store (0, std::memory_order_relaxed);
    g_mapsNotConstantBuffer.store (0, std::memory_order_relaxed);
    g_mapsTooLarge.store (0, std::memory_order_relaxed);
    g_mapsNoSlot.store (0, std::memory_order_relaxed);
    g_largestConstantBytes.store (0, std::memory_order_relaxed);
    g_windowsCaptured.store (0, std::memory_order_relaxed);
    g_windowsDropped.store (0, std::memory_order_relaxed);
    g_largestBoundOffset.store (0, std::memory_order_relaxed);
    g_frameId.store (0, std::memory_order_relaxed);
}

}   // namespace viewmatrix
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
