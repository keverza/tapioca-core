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
std::atomic<uint64_t> g_scenePassGeneration { 0 };
uint64_t g_sceneConsumedCount = 0;
// ⚠️ WHICH OF THE TWO PATHS SAID "CONSUMED", because
// they are not interchangeable and one log flag could not tell them apart.
// `consumed` is set BOTH by a copy whose source is the scene colour AND by the
// render-target departure that accepts a pass as the scene. Only the FIRST is a
// latch point -- a place where the scene is finished, the consumer has not run
// yet, and anything drawn lands in the image about to be presented. Run
// twenty-one measured zero of thirty-six copies sourcing the scene, and if that
// still holds then every `consumed=yes` in the log is the departure path and
// there is nothing to hang an in-pass injection on.
uint64_t g_consumedByCopy = 0;
uint64_t g_consumedByDeparture = 0;
uint64_t g_copiesSeenInPass = 0;
uint64_t g_modelSceneGeneration = 0;
uint64_t g_modelPassOfGeneration = 0;
DepartureStats g_departures;
SceneSignature g_signature;

// ⚠️ HOW MANY CONSECUTIVE FRAMES MUST AGREE. Two is enough to reject a one-off
// and cheap enough that the triangle appears almost immediately.
constexpr uint32_t kStableFramesRequired = 2;

// The passes completed in the frame being watched. Sixteen is more separate
// render passes than any capture has shown in one Archicad frame.
constexpr size_t kMaxPassesPerFrame = 16;
ScenePass g_framePasses[kMaxPassesPerFrame];
size_t g_framePassCount = 0;

// ⚠️ A FLOOR UNDER THE LEARNED THRESHOLD. A frame in which Archicad genuinely
// draws almost nothing must not make a four-draw gizmo pass look like the
// scene; the 3D pass of even a trivial model is busier than this.
constexpr uint32_t kMinimumSceneDraws = 24;
uint64_t g_boundColourResource = 0;

// ⚠️ ONE `GetResource` PER TARGET POINTER, AND THE REFERENCE IS RELEASED AT
// ONCE. `GetResource` AddRefs; holding that reference would keep Archicad's
// scene texture alive past a resize and is exactly the persistent cache stage 5
// forbids. The address is kept only as an identity to compare against.
uint64_t ResourceBehind (ID3D11RenderTargetView* view)
{
    if (view == nullptr)
        return 0;
    ID3D11Resource* resource = nullptr;
    view->GetResource (&resource);
    if (resource == nullptr)
        return 0;
    const uint64_t identity = uint64_t (uintptr_t (resource));
    resource->Release ();
    return identity;
}

ScenePass g_currentPass;
ScenePass g_lastCompletedPass;
uint64_t g_presentFrameId = 0;

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
std::atomic<uint64_t> g_reserved { 0 };
std::atomic<uint64_t> g_published { 0 };
std::atomic<uint64_t> g_drained { 0 };
std::atomic<uint64_t> g_framesClosed { 0 };
std::atomic<uint64_t> g_framesDropped { 0 };

uint64_t MicrosecondsNow ()
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    if (!QueryPerformanceFrequency (&frequency) || frequency.QuadPart == 0 || !QueryPerformanceCounter (&counter))
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
        return; // the histogram saturates; the counts above stay honest
    g_current.distinct[g_current.distinctCount] = viewport;
    g_current.distinctHits[g_current.distinctCount] = 1;
    ++g_current.distinctCount;
}

} // namespace

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
    g_boundColourResource = ResourceBehind (colour);
    g_boundDepth = uint64_t (uintptr_t (depth));

    // ⚠️ THE CANDIDATE INJECTION BOUNDARY. The colour target the current pass was
    // drawing into has just been bound away, and it had real draws in it -- so
    // the scene pass is over, its camera is still the last one bound, and its
    // depth buffer is still intact. Everything after this and before Present is
    // what stage 5 has to prove it lands in front of.
    if (g_currentPass.generation != 0 && !g_currentPass.boundaryHit && g_currentPass.draws > 0 &&
        previous == g_currentPass.colorTarget && g_boundColour != g_currentPass.colorTarget) {
        g_currentPass.boundaryHit = true;
        g_lastCompletedPass = g_currentPass;
    }
    else if (g_currentPass.generation != 0 && g_currentPass.boundaryHit && g_boundColour == g_currentPass.colorTarget) {
        // ⚠️ IT CAME BACK. The switch we called a boundary was not the end of the
        // scene, and anything injected there would have landed in the middle of
        // it. The pass reopens, and the count is what says the boundary to use is
        // a later switch than this one.
        ++g_currentPass.targetReturns;
        ++g_currentPass.targetEpoch;
        g_currentPass.drawsThisEpoch = 0;
        g_currentPass.boundaryHit = false;
        if (g_lastCompletedPass.generation == g_currentPass.generation)
            g_lastCompletedPass = g_currentPass;
    }
}

// Two viewports are the same rectangle if they agree to within half a pixel;
// exact float equality across frames is not something to rely on.
bool SameExtent (float a, float b)
{
    const float difference = (a > b) ? (a - b) : (b - a);
    return difference < 0.5f;
}

bool OnDraw ()
{
    if (g_currentPass.generation == 0)
        return false;
    if (!g_currentPass.boundaryHit && g_boundColour == g_currentPass.colorTarget) {
        ++g_currentPass.draws;
        ++g_currentPass.drawsThisEpoch;
        // ⚠️ DID THIS DRAW CARRY A CAMERA? That is what separates the model pass
        // from a gizmo pass on a host where neither is busy.
        {
            const contextstate::ContextState live = contextstate::Snapshot ();
            if (live.vsConstantBuffers[0].IsBound ()) {
                const uint64_t b0 = uint64_t (live.vsConstantBuffers[0].firstConstant);
                if (!g_currentPass.b0Bound) {
                    g_currentPass.b0Bound = true;
                    g_currentPass.b0WindowFirst = b0;
                    g_currentPass.b0NumConstants = live.vsConstantBuffers[0].numConstants;
                }
                else if (b0 != g_currentPass.b0WindowFirst) {
                    ++g_currentPass.b0WindowChanges;
                }
            }
            if (live.vsConstantBuffers[1].IsBound () && live.vsConstantBuffers[2].IsBound ()) {
                g_currentPass.drawsHadCamera = true;
                // ⚠️ THE WINDOW PAIR IS THE CAMERA'S ADDRESS.
                // Archicad keeps its constants in one ring and binds windows of
                // it, so a pair of first-constant offsets identifies which 256
                // bytes this draw multiplied by. See ScenePass.
                const uint64_t window = (uint64_t (live.vsConstantBuffers[1].firstConstant) << 32) |
                                        uint64_t (live.vsConstantBuffers[2].firstConstant);
                if (g_currentPass.cameraWindowFirst == 0 && g_currentPass.cameraWindowChanges == 0) {
                    g_currentPass.cameraWindowFirst = window;
                    g_currentPass.cameraWindowLast = window;
                }
                else if (window != g_currentPass.cameraWindowLast) {
                    g_currentPass.cameraWindowLast = window;
                    ++g_currentPass.cameraWindowChanges;
                    g_currentPass.cameraWindowLastChangeDraw = g_currentPass.draws;
                }
            }
        }
        // ⚠️ LATCHED AT THE DRAW, NOT AT THE BOUNDARY. Archicad may legally
        // change shader and constant-buffer state between its final scene draw
        // and the target switch, so the state live at the switch is not
        // necessarily the state that rendered the model. See
        // ContextStateTracker.hpp's `SceneDrawState`.
        // ⚠️ "IN THE MODEL PASS" MEANS THIS PASS MATCHES THE LEARNED SIGNATURE,
        // not merely that it cleared depth. Until phase 1 has learned one,
        // nothing is in the model pass and nothing is latched -- fail closed.
        const bool inModelPass = g_signature.learned && g_currentPass.colorResource == g_signature.colorResource &&
                                 SameExtent (g_currentPass.viewport.width, g_signature.viewportWidth) &&
                                 SameExtent (g_currentPass.viewport.height, g_signature.viewportHeight);
        // ⚠️ ONE INCREMENT PER MODEL PASS, AT ITS FIRST CAMERA-BEARING DRAW. A
        // generation per draw would make every later draw of the same scene look
        // like a new scene; a generation per Present is the mistake this replaces.
        if (inModelPass && g_modelPassOfGeneration != g_currentPass.generation) {
            g_modelPassOfGeneration = g_currentPass.generation;
            ++g_modelSceneGeneration;
        }
        const bool latchedCamera =
            contextstate::OnSceneDraw (g_currentPass.generation, g_currentPass.targetEpoch,
                                       g_currentPass.drawsThisEpoch, g_modelSceneGeneration, inModelPass);
        // ⚠️ TRUE MEANS "SNAPSHOT THE CAMERA THIS DRAW JUST BECAME THE SOURCE OF",
        // and the answer comes from the latch itself rather than from a second
        // predicate here. Run twenty-eight still reported exactly twice the truth
        // -- 724 qualifying draws against 362 camera-bearing ones -- because the
        // copy of the test on this side asked only whether `b1` and `b2` were
        // bound, while the latch also requires the 256-byte window. Two spellings
        // of one rule is one spelling too many.
        //
        // The binding tells you where the bytes are; only a copy preserves what
        // they were. See InjectionRenderer::SnapshotCamera.
        return latchedCamera;
    }
    // ⚠️ COUNTED SEPARATELY, NOT IGNORED. Draws after the boundary are the UI and
    // post passes, and how many there are decides whether the boundary is a
    // quiet place to inject or the middle of somebody else's work.
    ++g_currentPass.drawsAfterBoundary;
    if (g_currentPass.targetReturns > 0)
        ++g_currentPass.drawsAfterReturn;
    if (g_lastCompletedPass.generation == g_currentPass.generation)
        g_lastCompletedPass.drawsAfterBoundary = g_currentPass.drawsAfterBoundary;
    return false;
}

bool SceneCompletesAt (uint64_t newColorTarget)
{
    if (g_currentPass.generation == 0 || g_currentPass.draws == 0)
        return false;
    if (g_boundColour != g_currentPass.colorTarget)
        return false;
    if (newColorTarget == g_currentPass.colorTarget)
        return false;

    ++g_departures.departuresSeen;
    if (g_currentPass.sceneConsumed) {
        ++g_departures.rejectedAlreadyDone;
        return false;
    }

    // ⚠️ A DEPTH CLEAR STARTS A PASS, AND MOST PASSES ARE NOT THE 3D SCENE.
    // Run twenty-two: 1453 depth clears and 14178 indexed draws, while the pass
    // that happened to be current at the departure held FOUR of them. Archicad
    // clears depth many times a frame -- gizmos, highlights, overlays -- so "the
    // most recent depth clear" selects a small pass far more often than it
    // selects the scene, and the first triangle was injected into one of those
    // and then painted over by the real geometry. That is why it executed 595
    // times and was never seen.
    //
    // ⚠️ THE DISCRIMINATOR IS DRAW COUNT, LEARNED ONLINE AND FAIL-CLOSED. The 3D
    // scene is by a wide margin the busiest pass in its frame, so a pass
    // qualifies only once it has at least half as many draws as the busiest pass
    // seen so far, and never fewer than a floor. Until something has been seen
    // to be busy, NOTHING qualifies -- which costs the first frame or two and
    // cannot inject into the wrong pass.
    if (g_currentPass.draws > g_departures.busiestPassDraws)
        g_departures.busiestPassDraws = g_currentPass.draws;

    // ⚠️ THE LEARNED SIGNATURE, NOT A THRESHOLD. See `SceneSignature`: on this
    // host nothing is ever busy -- the busiest pass of a whole session had four
    // draws -- so size identifies nothing and the signature is everything. Until
    // phase 1 has agreed with itself on consecutive frames, this refuses.
    if (!g_signature.learned) {
        ++g_departures.rejectedTooFewDraws;
        return false;
    }
    g_departures.drawThreshold = (g_signature.draws > 1) ? (g_signature.draws / 2) : 1;
    const bool matches = g_currentPass.colorResource == g_signature.colorResource &&
                         g_currentPass.depthTarget == g_signature.depthTarget &&
                         SameExtent (g_currentPass.viewport.width, g_signature.viewportWidth) &&
                         SameExtent (g_currentPass.viewport.height, g_signature.viewportHeight) &&
                         g_currentPass.drawsHadCamera && g_currentPass.draws >= g_departures.drawThreshold;
    if (!matches) {
        ++g_departures.rejectedTooFewDraws;
        return false;
    }

    g_currentPass.sceneConsumed = true;
    if (g_lastCompletedPass.generation == g_currentPass.generation)
        g_lastCompletedPass.sceneConsumed = true;
    ++g_sceneConsumedCount;
    ++g_consumedByDeparture;
    ++g_departures.acceptedAsScene;
    return true;
}

DepartureStats GetDepartureStats ()
{
    return g_departures;
}

SceneSignature GetSceneSignature ()
{
    return g_signature;
}

void ForgetSceneSignature ()
{
    g_signature = SceneSignature {};
    g_framePassCount = 0;
}

bool OnCopyOrResolve (uint64_t sourceResource)
{
    if (g_currentPass.generation == 0)
        return false;
    if (g_currentPass.boundaryHit) {
        ++g_currentPass.opsAfterBoundary;
        if (g_lastCompletedPass.generation == g_currentPass.generation)
            g_lastCompletedPass.opsAfterBoundary = g_currentPass.opsAfterBoundary;
    }

    // ⚠️ SOURCE RESOURCE, NOT VIEW POINTER, and once per pass. Several views can
    // address one texture; and a scene consumed twice is one scene, so the
    // trigger fires on the FIRST consumer and not on every later copy of the
    // same bytes.
    // Every copy Archicad makes while a pass is open, so "no copy sourced the
    // scene" can be told from "no copy happened at all".
    ++g_copiesSeenInPass;
    if (sourceResource == 0 || sourceResource != g_currentPass.colorResource)
        return false;
    if (g_currentPass.sceneConsumed)
        return false;
    g_currentPass.sceneConsumed = true;
    if (g_lastCompletedPass.generation == g_currentPass.generation)
        g_lastCompletedPass.sceneConsumed = true;
    ++g_sceneConsumedCount;
    ++g_consumedByCopy;
    return true;
}

LatchCensus GetLatchCensus ()
{
    LatchCensus census;
    census.consumedByCopy = g_consumedByCopy;
    census.consumedByDeparture = g_consumedByDeparture;
    census.copiesSeenInPass = g_copiesSeenInPass;
    return census;
}

CompositeDraw g_composite;

void NoteDirectDraw (uint32_t vertexCount)
{
    // A fullscreen quad is four vertices, or six for two triangles. Anything
    // larger is geometry and not a composite.
    if (vertexCount > 6 || g_currentPass.generation == 0)
        return;
    const contextstate::ContextState live = contextstate::Snapshot ();
    const uint64_t b0 =
        live.vsConstantBuffers[0].IsBound () ? uint64_t (live.vsConstantBuffers[0].firstConstant) + 1 : 0;
    const uint64_t b1 =
        live.vsConstantBuffers[1].IsBound () ? uint64_t (live.vsConstantBuffers[1].firstConstant) + 1 : 0;
    const uint64_t b2 =
        live.vsConstantBuffers[2].IsBound () ? uint64_t (live.vsConstantBuffers[2].firstConstant) + 1 : 0;
    ++g_composite.draws;
    if (b0 != g_composite.b0Window || b1 != g_composite.b1Window || b2 != g_composite.b2Window)
        ++g_composite.windowChanges;
    g_composite.vertexCount = vertexCount;
    g_composite.b0Window = b0;
    g_composite.b1Window = b1;
    g_composite.b2Window = b2;
    g_composite.b0Bound = b0 != 0;
    g_composite.b1Bound = b1 != 0;
    g_composite.b2Bound = b2 != 0;
}

CompositeDraw GetCompositeDraw ()
{
    return g_composite;
}

uint64_t SceneConsumedCount ()
{
    return g_sceneConsumedCount;
}

uint64_t CurrentPresentGeneration ()
{
    return g_presentFrameId;
}

uint64_t ModelSceneGeneration ()
{
    return g_modelSceneGeneration;
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
uint64_t g_lastSceneColour = 0;
uint64_t g_lastSceneDepth = 0;
uint32_t g_lastSceneAge = 0;

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
    pass.targetEpoch = 1;
    pass.colorResource = g_boundColourResource;

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
    if (g_currentPass.generation != 0 && !g_currentPass.boundaryHit && g_currentPass.draws > 0) {
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

    // ---- phase 1: pick this frame's model pass, and see if it is stable ----
    if (g_currentPass.generation != 0 && g_framePassCount < kMaxPassesPerFrame)
        g_framePasses[g_framePassCount++] = g_currentPass;

    if (g_framePassCount > 0) {
        ++g_signature.framesWatched;

        // ⚠️ THE CANDIDATE IS THE BIGGEST FULL-VIEWPORT PASS WHOSE DRAWS CARRIED
        // A CAMERA. All three conditions matter: a gizmo pass has a camera but a
        // small viewport or few draws, a clear-only pass has neither, and a
        // full-screen post pass has no camera at all.
        const ScenePass* best = nullptr;
        float widest = 0.0f;
        for (size_t i = 0; i < g_framePassCount; ++i) {
            if (g_framePasses[i].viewport.width > widest)
                widest = g_framePasses[i].viewport.width;
        }
        uint32_t candidates = 0;
        for (size_t i = 0; i < g_framePassCount; ++i) {
            const ScenePass& pass = g_framePasses[i];
            if (!pass.drawsHadCamera || pass.draws == 0)
                continue;
            if (!SameExtent (pass.viewport.width, widest))
                continue;
            ++candidates;
            if (best == nullptr || pass.draws > best->draws)
                best = &pass;
        }
        g_signature.candidatesThisFrame = candidates;

        if (best == nullptr) {
            g_signature.stableFrames = 0;
        }
        else if (g_signature.colorResource == best->colorResource && g_signature.depthTarget == best->depthTarget &&
                 SameExtent (g_signature.viewportWidth, best->viewport.width) &&
                 SameExtent (g_signature.viewportHeight, best->viewport.height)) {
            if (g_signature.stableFrames < 0xffffffffu)
                ++g_signature.stableFrames;
            // Track the typical draw count rather than the first one seen.
            g_signature.draws = (g_signature.draws + best->draws) / 2;
            if (g_signature.stableFrames >= kStableFramesRequired)
                g_signature.learned = true;
        }
        else {
            // A different signature: start counting again from this one.
            g_signature.colorResource = best->colorResource;
            g_signature.depthTarget = best->depthTarget;
            g_signature.viewportWidth = best->viewport.width;
            g_signature.viewportHeight = best->viewport.height;
            g_signature.draws = best->draws;
            g_signature.stableFrames = 1;
            g_signature.learned = false;
        }
        g_framePassCount = 0;
    }

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
    }
    else {
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
    g_sceneConsumedCount = 0;
    g_modelSceneGeneration = 0;
    g_modelPassOfGeneration = 0;
    g_departures = DepartureStats {};
    g_signature = SceneSignature {};
    g_framePassCount = 0;
    g_boundColourResource = 0;
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
            navlog::LogGpuFrame (sessionMs, frame.frameId, frame.largest.x, frame.largest.y, frame.largest.width,
                                 frame.largest.height, frame.sceneCandidate.x, frame.sceneCandidate.y,
                                 frame.sceneCandidate.width, frame.sceneCandidate.height, frame.sceneCandidate.minDepth,
                                 frame.sceneCandidate.maxDepth, frame.sceneColorTarget, frame.sceneDepthTarget,
                                 frame.viewportSets, frame.distinctCount, frame.depthClears);
        }
        if (count < 64)
            break;
    }
}

} // namespace renderstate
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
