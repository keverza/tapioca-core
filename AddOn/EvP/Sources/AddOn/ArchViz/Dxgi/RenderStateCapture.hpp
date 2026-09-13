#ifndef EVP_ARCHVIZ_DXGI_RENDERSTATECAPTURE_HPP
#define EVP_ARCHVIZ_DXGI_RENDERSTATECAPTURE_HPP

// The viewport ARCHICAD actually rendered with, per frame (PLAT-RE154,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 2). DISCOVERY ONLY.
//
// WHY THE GPU STATE AND NOT THE WINDOW. Everything the overlay currently knows
// about where Archicad's 3D view sits on screen comes from window rectangles --
// `CameraWake::PointerOverView` tracks one by hit-testing the pointer. A window
// rectangle is a good guess and it is wrong in all the ways that matter here:
// it does not know about the DPI scale the swap chain was created at, it does
// not know that `ResizeBuffers` can be REFUSED while a back-buffer reference is
// outstanding (PLAT-RE150) so the requested size and the live size differ for
// some frames, and it does not know which sub-rectangle of the window the 3D
// pass was actually rasterised into. Turning a captured view-projection into
// pixels needs the rectangle the rasteriser used, not the one the window
// manager reports. Where they disagree, the GPU wins; the tracked window rect
// stays useful as a sanity check and is logged beside it for exactly that.
//
// ⚠️ A FRAME SETS MANY VIEWPORTS AND ONLY ONE OF THEM IS THE 3D VIEW. Archicad
// draws its own UI, thumbnails, and any number of small passes through the same
// context, so "the last viewport before Present" is whatever it happened to
// finish with -- usually a widget. Two candidates are recorded instead, and
// which one is right is a question stage 3 answers rather than one this file
// decides:
//
//   * the LARGEST viewport of the frame, which is the 3D view in a normal
//     window and is the wrong answer when a full-window UI pass follows it;
//   * the viewport in effect at the last DEPTH CLEAR, which is a much stronger
//     signal -- a depth buffer is cleared by a 3D scene pass and by very little
//     else -- and is the wrong answer if Archicad clears depth for a gizmo.
//
// Recording both, with the target and depth-target pointers bound at that
// moment, is what lets the classification in stage 3 be scored rather than
// assumed. ⚠️ DO NOT COLLAPSE THEM TO ONE "the viewport" FIELD before that
// scoring exists; a single field would have to pick, and picking here is the
// mistake this comment is here to prevent.
//
// ⚠️ THE FRAME IS CLOSED AT `Present`, WHICH IS AFTER ARCHICAD'S DRAWS. That is
// the correct boundary for stage 2 -- the state accumulated between two Presents
// IS one frame -- but it is emphatically NOT the injection point stage 5 looks
// for. Nothing here should be read as "this is where the overlay draws".
//
// THREAD SAFETY. The `On*` functions run inside the context detours, on
// Archicad's render thread, and `OnPresent` inside the present detour. They
// allocate nothing, take no lock and never call ACAPI.
//
// ⚠️ THE ACCUMULATOR IS PLAIN, NOT ATOMIC, AND THAT IS A REASONED CHOICE. D3D11's
// own contract makes an immediate context single-threaded, and the context hook
// only ever forwards Archicad's immediate context here, so there is exactly one
// writer. Present is called from that same render thread in every capture taken
// so far. If Archicad ever presents from a different thread than it renders on,
// the cost is a snapshot that mixes two frames' scalars -- a wrong row in a
// diagnostic, never a crash and never a torn pointer, because every field is a
// naturally-aligned scalar. Making it atomic would put an interlocked write on a
// path that runs thousands of times per frame to buy a guarantee this data does
// not need.

#include <cstdint>

struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct D3D11_VIEWPORT;
typedef struct tagRECT RECT;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace renderstate {

struct GpuViewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 0.0f;

    float Area () const { return width * height; }
};

// How many distinct viewports one frame's histogram holds. Eight is more passes
// than any capture has shown and keeps the frame record small enough to ring
// hundreds of them.
constexpr size_t kMaxDistinctViewports = 8;

struct FrameState {
    bool        valid = false;
    uint64_t    frameId = 0;
    uint64_t    timestampUs = 0;

    GpuViewport largest;          // biggest area seen this frame
    GpuViewport sceneCandidate;   // whatever was current at the last depth clear
    uint64_t    sceneColorTarget = 0;
    uint64_t    sceneDepthTarget = 0;

    GpuViewport lastViewport;     // current at Present; usually a UI pass
    uint64_t    lastColorTarget = 0;
    uint64_t    lastDepthTarget = 0;

    int32_t     scissorLeft = 0;
    int32_t     scissorTop = 0;
    int32_t     scissorRight = 0;
    int32_t     scissorBottom = 0;

    uint32_t    viewportSets = 0;
    uint32_t    targetBinds = 0;
    uint32_t    colourClears = 0;
    uint32_t    depthClears = 0;

    // ⚠️ HOW MANY FRAMES AGO THE SCENE VIEWPORT WAS ACTUALLY SEEN, and it is not
    // a diagnostic nicety. `sceneCandidate` is sampled at a depth clear, and
    // Archicad presents frames that clear nothing -- at rest it presents
    // thousands of them, so the LAST frame in the ring had a zeroed candidate
    // and run fifteen announced "NO SCENE VIEWPORT" while its own slot table
    // showed 2394 depth clears. The candidate is now carried forward and this
    // says how stale it is: 0 means this frame drew the scene, a large number
    // means nothing has drawn a 3D pass for a while, and only the second is the
    // finding the old message was trying to report.
    uint32_t    sceneCandidateAgeFrames = 0;

    uint32_t    distinctCount = 0;
    GpuViewport distinct[kMaxDistinctViewports];
    uint32_t    distinctHits[kMaxDistinctViewports] = {};
};

// ---- the scene pass (stage 4/5) --------------------------------------------
// ⚠️ A PRESENT INTERVAL IS NOT A RENDERING PASS, AND PAIRING ON ONE IS WRONG.
// Between two Presents Archicad may run a shadow pass, a preview, a thumbnail, a
// selection-highlight pass and its UI -- each with its own camera and its own
// targets. A view matrix from one and a projection from another carry the same
// Present frame id and describe different things, and their product is a
// plausible matrix that is wrong in a way no still-view pixel test can catch.
//
// So the identity a captured camera needs is not `view.frame == projection.frame`
// but:
//
//     Present generation
//         scene-pass generation          <- this counter
//             viewport
//             RTV / DSV
//             view binding
//             projection binding
//             final scene draw
//
// ⚠️ A PASS BEGINS AT A DEPTH CLEAR, which is the strongest signal available
// without hooking every draw, and ENDS when the colour target is bound away from
// the one that clear was aimed at. That end is also stage 5's candidate
// injection boundary: the last scene draw has happened, the camera for it is
// still bound, and the depth buffer still holds the scene -- so geometry drawn
// there is occluded correctly.
//
// ⚠️ WHETHER THAT BOUNDARY REALLY PRECEDES RESOLVE AND POST-PROCESSING IS NOT
// ASSUMED. `opsAfterBoundary` counts the copies and resolves seen between the
// boundary and Present; a non-zero count with the scene resources still bound is
// the evidence that injecting there lands before them, and a zero count means
// the boundary is at or after the post chain and stage 5 must move earlier.
struct ScenePass {
    uint64_t    generation = 0;      // 0 until the first depth clear
    uint64_t    presentFrameId = 0;  // which Present interval it lived in
    uint64_t    colorTarget = 0;
    uint64_t    depthTarget = 0;
    GpuViewport viewport;
    uint32_t    draws = 0;           // draw calls seen while its target was bound
    bool        boundaryHit = false; // the colour target was bound away afterwards
    uint32_t    opsAfterBoundary = 0;// copies/resolves between boundary and Present
    uint32_t    drawsAfterBoundary = 0;

    // ⚠️ HOW MANY TIMES ARCHICAD CAME BACK TO THIS TARGET AFTER "LEAVING" IT.
    // The candidate injection boundary is the switch away from the scene target,
    // and it is the FINAL boundary only if the target never returns. Run
    // eighteen recorded four draws into the target, a switch, and then eight
    // more draws -- so treating the first switch as the end of the scene would
    // have injected into the middle of it. A non-zero count here means the
    // boundary to use is a later switch, not this one.
    uint32_t    targetReturns = 0;
    uint32_t    drawsAfterReturn = 0;

    // The bindings that were live when this pass BEGAN -- inherited, not waited
    // for. See ContextStateTracker.hpp: a camera constant bound once and never
    // rebound is invisible to anything that only records events after the depth
    // clear, which is the likeliest reason the view matrix has been found once
    // in four runs while the projection is found every time.
    uint64_t    vsShaderAtPassStart = 0;
    uint64_t    vsBuffer[14] = {};
    uint32_t    vsFirstConstant[14] = {};
};

// The most recently STARTED pass, and the most recently COMPLETED one. A
// consumer that wants to draw wants the completed one; a consumer that wants to
// know what is happening right now wants the started one.
ScenePass CurrentScenePass ();
ScenePass LastCompletedScenePass ();

// Render thread. The draw detours call this so a pass knows how much work went
// into it -- `draws` is what separates the 3D pass from a one-draw gizmo that
// also cleared depth.
void OnDraw ();
void OnCopyOrResolve ();

// ---- render thread, from the context detours -------------------------------
void OnViewport (const D3D11_VIEWPORT& viewport);
void OnScissor (const RECT& rect);
void OnRenderTargets (ID3D11RenderTargetView* colour, ID3D11DepthStencilView* depth);
void OnClearRenderTarget (ID3D11RenderTargetView* view);
void OnClearDepthStencil (ID3D11DepthStencilView* view);

// ---- render thread, from the present detour --------------------------------
// Close the frame: publish the accumulated state and start a new one. `frameId`
// is the present hook's running count for Archicad's chain, so a frame record
// and a present row name the same frame.
void OnPresent (uint64_t frameId);

// ---- main thread -----------------------------------------------------------
// The most recently closed frame. `valid` false until one has been.
FrameState LatestFrame ();

// Copy out up to `max` frames not yet drained, oldest first.
size_t DrainFrames (FrameState* out, size_t max);

struct CaptureStats {
    uint64_t framesClosed = 0;
    uint64_t framesDropped = 0;   // ring overrun before the main thread drained
    // The last closed frame's two candidates, so a diagnostic can print the
    // answer without the caller reassembling it.
    GpuViewport largest;
    GpuViewport sceneCandidate;
    // How stale that candidate is, in frames. See FrameState's field.
    uint32_t    sceneCandidateAgeFrames = 0;
    uint32_t    distinctCount = 0;
};
CaptureStats GetCaptureStats ();

// MAIN THREAD. Clear everything. Called when the hook is installed, so a run
// never inherits the previous one's frames.
void Reset ();

// Write drained frames to the nav log as `source=gpuframe` rows. MAIN THREAD.
void FlushFrameLog ();

}   // namespace renderstate
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
