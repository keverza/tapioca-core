// ArchViz/Dxgi/RenderStateCapture -- see the header. Every rule about this file
// is in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#include "ArchViz/NavLog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace renderstate {

namespace {

// The frame being accumulated. Plain, one writer -- see the header for why that
// is a reasoned choice rather than an oversight.
FrameState g_current;

// Bound right now, carried into whichever candidate needs them.
uint64_t g_boundColour = 0;
uint64_t g_boundDepth = 0;
GpuViewport g_currentViewport;

// ---- the closed-frame ring -------------------------------------------------
// Reserve/fill/publish, and a drain counter, exactly as `ContextHook`'s does.
// 256 frames is four seconds at 60 Hz, which is comfortably longer than the gap
// between camera ticks that drain it.
constexpr size_t kRingSize = 256;
FrameState g_ring[kRingSize] = {};
std::atomic<uint64_t> g_reserved {0};
std::atomic<uint64_t> g_published {0};
std::atomic<uint64_t> g_drained {0};
std::atomic<uint64_t> g_framesClosed {0};
std::atomic<uint64_t> g_framesDropped {0};

uint64_t MicrosecondsNow ()
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    if (!QueryPerformanceFrequency (&frequency) || frequency.QuadPart == 0 ||
        !QueryPerformanceCounter (&counter))
        return 0;
    return uint64_t (counter.QuadPart * 1000000ll / frequency.QuadPart);
}

// ⚠️ COMPARED WITH A TOLERANCE, NOT WITH ==. Viewport dimensions are floats that
// come from Archicad's own arithmetic, and at a fractional DPI scale the same
// logical rectangle can arrive as 1706.6666 one frame and 1706.6667 the next.
// Exact comparison would fill the eight-slot histogram with one viewport wearing
// eight faces and report the frame as having eight passes.
bool SameViewport (const GpuViewport& a, const GpuViewport& b)
{
    const float tolerance = 0.5f;
    return std::fabs (a.x - b.x) < tolerance && std::fabs (a.y - b.y) < tolerance &&
           std::fabs (a.width - b.width) < tolerance && std::fabs (a.height - b.height) < tolerance;
}

void CountDistinct (const GpuViewport& viewport)
{
    for (uint32_t i = 0; i < g_current.distinctCount; ++i) {
        if (SameViewport (g_current.distinct[i], viewport)) {
            ++g_current.distinctHits[i];
            return;
        }
    }
    if (g_current.distinctCount >= kMaxDistinctViewports)
        return;   // the histogram saturates; the counts above stay honest
    g_current.distinct[g_current.distinctCount] = viewport;
    g_current.distinctHits[g_current.distinctCount] = 1;
    ++g_current.distinctCount;
}

}   // namespace

void OnViewport (const D3D11_VIEWPORT& viewport)
{
    g_currentViewport.x = viewport.TopLeftX;
    g_currentViewport.y = viewport.TopLeftY;
    g_currentViewport.width = viewport.Width;
    g_currentViewport.height = viewport.Height;
    g_currentViewport.minDepth = viewport.MinDepth;
    g_currentViewport.maxDepth = viewport.MaxDepth;

    ++g_current.viewportSets;
    g_current.lastViewport = g_currentViewport;
    if (g_currentViewport.Area () > g_current.largest.Area ())
        g_current.largest = g_currentViewport;
    CountDistinct (g_currentViewport);
}

void OnScissor (const RECT& rect)
{
    g_current.scissorLeft = rect.left;
    g_current.scissorTop = rect.top;
    g_current.scissorRight = rect.right;
    g_current.scissorBottom = rect.bottom;
}

void OnRenderTargets (ID3D11RenderTargetView* colour, ID3D11DepthStencilView* depth)
{
    ++g_current.targetBinds;
    g_boundColour = uint64_t (uintptr_t (colour));
    g_boundDepth = uint64_t (uintptr_t (depth));
}

void OnClearRenderTarget (ID3D11RenderTargetView* view)
{
    (void) view;
    ++g_current.colourClears;
}

void OnClearDepthStencil (ID3D11DepthStencilView* view)
{
    ++g_current.depthClears;
    // ⚠️ THE VIEWPORT IS SAMPLED HERE, not at the next draw. A depth clear is the
    // strongest available signal that a 3D scene pass is starting, and the
    // viewport current at that instant is the one that pass will rasterise into.
    // Waiting for a draw call would mean hooking one of the hottest functions in
    // the API to learn something this already knows.
    g_current.sceneCandidate = g_currentViewport;
    g_current.sceneColorTarget = g_boundColour;
    // The DSV being cleared, not merely the one bound -- they are the same in
    // every sane frame, and where they differ the one being cleared is the one
    // the pass is about to use.
    g_current.sceneDepthTarget = (view != nullptr) ? uint64_t (uintptr_t (view)) : g_boundDepth;
}

void OnPresent (uint64_t frameId)
{
    g_current.valid = true;
    g_current.frameId = frameId;
    g_current.timestampUs = MicrosecondsNow ();
    g_current.lastColorTarget = g_boundColour;
    g_current.lastDepthTarget = g_boundDepth;

    const uint64_t reserved = g_reserved.fetch_add (1, std::memory_order_relaxed);
    const uint64_t drained = g_drained.load (std::memory_order_acquire);
    if (reserved - drained < kRingSize) {
        g_ring[reserved % kRingSize] = g_current;
        g_published.fetch_add (1, std::memory_order_release);
    } else {
        // ⚠️ COUNTED, NOT SILENT. A dropped frame record is indistinguishable
        // from a frame Archicad did not draw, and "the viewport stopped
        // changing" is exactly the conclusion a resize test would draw from it.
        g_framesDropped.fetch_add (1, std::memory_order_relaxed);
    }
    g_framesClosed.fetch_add (1, std::memory_order_relaxed);

    // ⚠️ THE ACCUMULATOR IS RESET, THE BINDINGS ARE NOT. Viewports, targets and
    // clears are per-frame counts and must start at zero; what is BOUND survives
    // a Present, because D3D11 state is not reset by presenting and the next
    // frame really does begin with the previous frame's bindings in place.
    // Zeroing them here would make every frame's first pass look untargeted.
    const GpuViewport carried = g_currentViewport;
    g_current = FrameState {};
    g_currentViewport = carried;
}

FrameState LatestFrame ()
{
    const uint64_t published = g_published.load (std::memory_order_acquire);
    if (published == 0)
        return FrameState {};
    return g_ring[(published - 1) % kRingSize];
}

size_t DrainFrames (FrameState* out, size_t max)
{
    if (out == nullptr || max == 0)
        return 0;
    const uint64_t published = g_published.load (std::memory_order_acquire);
    uint64_t drained = g_drained.load (std::memory_order_relaxed);
    size_t written = 0;
    while (drained < published && written < max) {
        out[written++] = g_ring[drained % kRingSize];
        ++drained;
    }
    g_drained.store (drained, std::memory_order_release);
    return written;
}

CaptureStats GetCaptureStats ()
{
    CaptureStats stats;
    stats.framesClosed = g_framesClosed.load (std::memory_order_relaxed);
    stats.framesDropped = g_framesDropped.load (std::memory_order_relaxed);
    const FrameState latest = LatestFrame ();
    stats.largest = latest.largest;
    stats.sceneCandidate = latest.sceneCandidate;
    stats.distinctCount = latest.distinctCount;
    return stats;
}

void Reset ()
{
    g_reserved.store (0, std::memory_order_release);
    g_published.store (0, std::memory_order_release);
    g_drained.store (0, std::memory_order_release);
    g_framesClosed.store (0, std::memory_order_relaxed);
    g_framesDropped.store (0, std::memory_order_relaxed);
    g_current = FrameState {};
    g_currentViewport = GpuViewport {};
    g_boundColour = 0;
    g_boundDepth = 0;
}

void FlushFrameLog ()
{
    const uint64_t nowUs = MicrosecondsNow ();
    const uint64_t nowSessionMs = navlog::SessionNowMs ();

    FrameState batch[64];
    for (;;) {
        const size_t count = DrainFrames (batch, 64);
        if (count == 0)
            break;
        for (size_t i = 0; i < count; ++i) {
            const FrameState& frame = batch[i];
            if (frame.timestampUs == 0 || frame.timestampUs > nowUs)
                continue;
            const uint64_t agoMs = (nowUs - frame.timestampUs) / 1000ull;
            const uint64_t sessionMs = (nowSessionMs > agoMs) ? (nowSessionMs - agoMs) : 0;
            navlog::LogGpuFrame (sessionMs, frame.frameId,
                                 frame.largest.x, frame.largest.y,
                                 frame.largest.width, frame.largest.height,
                                 frame.sceneCandidate.x, frame.sceneCandidate.y,
                                 frame.sceneCandidate.width, frame.sceneCandidate.height,
                                 frame.sceneCandidate.minDepth, frame.sceneCandidate.maxDepth,
                                 frame.sceneColorTarget, frame.sceneDepthTarget,
                                 frame.viewportSets, frame.distinctCount, frame.depthClears);
        }
        if (count < 64)
            break;
    }
}

}   // namespace renderstate
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
