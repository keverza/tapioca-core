// ArchViz/Dxgi/RenderStateCapture -- see the header. Every rule about this file
// is in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"

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

// ---- the scene pass --------------------------------------------------------
std::atomic<uint64_t> g_scenePassGeneration {0};
ScenePass g_currentPass;
ScenePass g_lastCompletedPass;
uint64_t  g_presentFrameId = 0;


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
    const uint64_t previous = g_boundColour;
    g_boundColour = uint64_t (uintptr_t (colour));
    g_boundDepth = uint64_t (uintptr_t (depth));

    // ⚠️ THE CANDIDATE INJECTION BOUNDARY. The colour target the current pass was
    // drawing into has just been bound away, and it had real draws in it -- so
    // the scene pass is over, its camera is still the last one bound, and its
    // depth buffer is still intact. Everything after this and before Present is
    // what stage 5 has to prove it lands in front of.
    if (g_currentPass.generation != 0 && !g_currentPass.boundaryHit &&
        g_currentPass.draws > 0 && previous == g_currentPass.colorTarget &&
        g_boundColour != g_currentPass.colorTarget) {
        g_currentPass.boundaryHit = true;
        g_lastCompletedPass = g_currentPass;
    } else if (g_currentPass.generation != 0 && g_currentPass.boundaryHit &&
               g_boundColour == g_currentPass.colorTarget) {
        // ⚠️ IT CAME BACK. The switch we called a boundary was not the end of the
        // scene, and anything injected there would have landed in the middle of
        // it. The pass reopens, and the count is what says the boundary to use is
        // a later switch than this one.
        ++g_currentPass.targetReturns;
        g_currentPass.boundaryHit = false;
        if (g_lastCompletedPass.generation == g_currentPass.generation)
            g_lastCompletedPass = g_currentPass;
    }
}

void OnDraw ()
{
    if (g_currentPass.generation == 0)
        return;
    if (!g_currentPass.boundaryHit && g_boundColour == g_currentPass.colorTarget) {
        ++g_currentPass.draws;
        return;
    }
    // ⚠️ COUNTED SEPARATELY, NOT IGNORED. Draws after the boundary are the UI and
    // post passes, and how many there are decides whether the boundary is a
    // quiet place to inject or the middle of somebody else's work.
    ++g_currentPass.drawsAfterBoundary;
    if (g_currentPass.targetReturns > 0)
        ++g_currentPass.drawsAfterReturn;
    if (g_lastCompletedPass.generation == g_currentPass.generation)
        g_lastCompletedPass.drawsAfterBoundary = g_currentPass.drawsAfterBoundary;
}

void OnCopyOrResolve ()
{
    if (g_currentPass.generation == 0 || !g_currentPass.boundaryHit)
        return;
    ++g_currentPass.opsAfterBoundary;
    if (g_lastCompletedPass.generation == g_currentPass.generation)
        g_lastCompletedPass.opsAfterBoundary = g_currentPass.opsAfterBoundary;
}

ScenePass CurrentScenePass ()
{
    return g_currentPass;
}

ScenePass LastCompletedScenePass ()
{
    return g_lastCompletedPass;
}

void OnClearRenderTarget (ID3D11RenderTargetView* view)
{
    (void) view;
    ++g_current.colourClears;
}

// The last frame that actually drew a 3D pass, carried across frames that do
// not. See `sceneCandidateAgeFrames` in the header for why.
GpuViewport g_lastScene;
uint64_t    g_lastSceneColour = 0;
uint64_t    g_lastSceneDepth = 0;
uint32_t    g_lastSceneAge = 0;

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

    g_lastScene = g_current.sceneCandidate;
    g_lastSceneColour = g_current.sceneColorTarget;
    g_lastSceneDepth = g_current.sceneDepthTarget;
    g_lastSceneAge = 0;

    // ⚠️ A NEW SCENE PASS STARTS HERE. Every camera binding recorded from now
    // until the boundary belongs to THIS pass, and that is the association
    // stage 4 needs -- not the Present interval, which can hold several passes
    // with different cameras.
    ScenePass pass;
    pass.generation = g_scenePassGeneration.fetch_add (1, std::memory_order_relaxed) + 1;
    pass.presentFrameId = g_presentFrameId;
    pass.colorTarget = g_current.sceneColorTarget;
    pass.depthTarget = g_current.sceneDepthTarget;
    pass.viewport = g_current.sceneCandidate;

    // ⚠️ INHERIT, DO NOT WAIT. The pass takes whatever is bound at this instant
    // as its own starting state; requiring the camera buffers to be re-bound
    // after the depth clear is what made the view matrix look intermittent.
    const contextstate::ContextState live = contextstate::Snapshot ();
    pass.vsShaderAtPassStart = live.vertexShader;
    for (size_t i = 0; i < contextstate::kConstantBufferSlots && i < 14; ++i) {
        pass.vsBuffer[i] = live.vsConstantBuffers[i].buffer;
        pass.vsFirstConstant[i] = live.vsConstantBuffers[i].firstConstant;
    }

    g_currentPass = pass;
}

void OnPresent (uint64_t frameId)
{
    g_presentFrameId = frameId;
    // A pass that never had its target bound away still ended -- at Present.
    // Recording it as completed is what stops a frame whose UI draws into the
    // same target from losing its pass entirely.
    if (g_currentPass.generation != 0 && !g_currentPass.boundaryHit &&
        g_currentPass.draws > 0) {
        g_lastCompletedPass = g_currentPass;
    }

    // ⚠️ A FRAME THAT CLEARED NO DEPTH BUFFER INHERITS THE LAST ONE THAT DID.
    // Otherwise the answer to "what viewport does Archicad render the scene
    // with" depends on whether the very last present happened to be a 3D pass,
    // and at rest it never is -- which is how run fifteen reported NO SCENE
    // VIEWPORT beside its own count of 2394 depth clears.
    if (g_current.depthClears == 0) {
        if (g_lastSceneAge < 0xffffffffu)
            ++g_lastSceneAge;
        g_current.sceneCandidate = g_lastScene;
        g_current.sceneColorTarget = g_lastSceneColour;
        g_current.sceneDepthTarget = g_lastSceneDepth;
    }
    g_current.sceneCandidateAgeFrames = g_lastSceneAge;

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
    stats.sceneCandidateAgeFrames = latest.sceneCandidateAgeFrames;
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
    g_lastScene = GpuViewport {};
    g_lastSceneColour = 0;
    g_lastSceneDepth = 0;
    g_lastSceneAge = 0;
    g_scenePassGeneration.store (0, std::memory_order_relaxed);
    g_currentPass = ScenePass {};
    g_lastCompletedPass = ScenePass {};
    g_presentFrameId = 0;
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
