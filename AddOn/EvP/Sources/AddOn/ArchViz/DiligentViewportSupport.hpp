#ifndef EVP_ARCHVIZ_DILIGENTVIEWPORTSUPPORT_HPP
#define EVP_ARCHVIZ_DILIGENTVIEWPORTSUPPORT_HPP

// ArchViz/DiligentViewportSupport — the pieces of the viewport that are not the
// viewport's LIFECYCLE: Diligent's own diagnostic callback, the Archicad-camera
// conversion, the corner gnomon's pass, and the plan anchors' frame step.
//
// It is a separate translation unit because `DiligentViewport.cpp` crossed the
// ~1,000-line cap, and this is the seam that was already there. What is left in
// that file is one thing: start the thread, own the device/swap chain/scene, run
// the frame loop, tear it all down in the order the flip model demands. These
// these are each self-contained and each has exactly one caller.
//
// ⚠️ RENDER THREAD ONLY, like everything else in the Diligent viewport, with the
// single exception noted on ApplyArchicadCamera's `CameraStart` argument: the
// STRUCT must have been filled on the main thread, because reading it is ACAPI.
// Applying it is not.

#include "ArchViz/DiligentViewport.hpp"       // CameraStart, PlanAnchorVertex
#include "ArchViz/DiligentScene.hpp"          // DiligentSceneStats, for CopySceneStatsInto
#include "ArchViz/DiligentPickBuffer.hpp"     // PublishCompletedPick
#include "ArchViz/DiligentViewportTarget.hpp" // ApplyRequestedFrameLatency
#include "ArchViz/InputRingBuffer.hpp"        // InputSnapshot, for ServicePick

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

class Camera;
class DiligentScene;
class PlanAnchorLayer;
struct HudState;

void ApplyShadowSettings (DiligentScene& scene, const HudState& hud);

// Which of the scene's two passes this frame runs, and running it.
//
// ⚠️ IT IS HERE BECAUSE DiligentViewport.cpp CROSSED THE SOFT CAP AGAIN, and
// this is the seam that was already there: everything else left in that file is
// the frame LOOP -- acquire, present, resize, tear down -- while this is one
// self-contained decision with one caller, exactly like the gnomon pass and the
// plan-anchor step beside it.
//
// The decision itself is small and the reasons are not: a G-buffer debug view
// and the ordinary shaded path both need the G-buffer, but only the ordinary
// path prepares AMBIENT OCCLUSION from it, and whichever path runs must leave
// no occlusion standing for the next frame to multiply in.
struct SceneDrawRequest {
    Diligent::ITextureView* target = nullptr;
    // ⚠️ THE DEPTH VIEW TRAVELS WITH THE COLOUR ONE, because DiligentScene::Draw
    // binds both itself now -- see its header for the grey viewport that made
    // that necessary. It is bound, never cleared: the frame loop clears it once,
    // before navigation.
    Diligent::ITextureView* depth = nullptr;
    const float* view = nullptr;     // 16
    const float* proj = nullptr;     // 16
    const float* viewProj = nullptr; // 16
    // Unjittered current camera, used only to write motion vectors. Visible
    // geometry uses viewProj above.
    const float* motionViewProj = nullptr; // 16
    const float* eye = nullptr;            // 3
    const float* jitter = nullptr;         // 2
    int debugView = 0;
    uint32_t frameIndex = 0;
    // RE51.C3. `intensity` scales the darkening only, never the effect's radius.
    bool ambientOcclusion = true;
    float ambientOcclusionIntensity = 1.0f;
    // World metres; 0 derives it from the model. See SetAmbientOcclusion.
    float ambientOcclusionRadius = 0.0f;
    // RE51.C7. Screen-space reflections -- see SetScreenSpaceReflection.
    bool screenSpaceReflection = false;
    float ssrIntensity = 1.0f;
    float ssrRoughnessThreshold = 0.2f;
    // RE51.C8. TAA only runs in the HDR final path; this is the requested state.
    bool temporalAntiAliasing = false;
    float taaStability = 0.9f;
};

// `lastLoggedGBufferView` is carried by the caller so switching away from a
// debug view and back reports the camera as it is THEN rather than staying
// silent. -1 means "nothing logged yet".
void DrawSceneOrDebugView (DiligentScene& scene, Camera& camera, Diligent::IDeviceContext* context,
                           const SceneDrawRequest& request, int& lastLoggedGBufferView);

// Diligent's OWN diagnostics, into archviz.log.
//
// ⚠️ IT COST A ROUND TRIP NOT TO HAVE THIS. `CreateSwapChainD3D11 returned no
// swap chain` was all we could report, six times in a row, while Diligent itself
// had the HRESULT and the reason the whole time and was writing them to a
// callback nobody had installed. Install it before the first Diligent call.
void InstallDiligentDebugCallback ();

// Point a Camera at what Archicad's 3D window is pointing at.
//
// ⚠️ ONE COPY, USED BY BOTH THE START AND THE LIVE SYNC. When these were two,
// the starting view and a synced view could differ by a field-of-view
// conversion, and the difference only shows up as the overlay drifting out of
// register the first time Archicad's camera is pushed in -- long after the
// change that caused it.
//
// Returns false when there is no perspective camera to copy; the caller then
// frames the model instead of inventing one.
bool ApplyArchicadCamera (Camera& camera, const CameraStart& start, uint32_t width, uint32_t height,
                          float* outDistance = nullptr);

bool SnapshotPerspectiveCamera (const Camera& camera, uint32_t width, uint32_t height, CameraStart& snapshot);

// The axis gnomon, in a small square viewport in the bottom-left corner.
//
// ⚠️ IT BORROWS ONLY THE CAMERA'S *ORIENTATION*, not its position or its
// distance. The gnomon must turn exactly as the model turns and must not change
// size when the user zooms; slaving it to the whole camera would put it
// kilometres away on a site model and inside the arrows on a detail.
//
// ⚠️ IT CLEARS THE WHOLE DEPTH BUFFER, so it must run AFTER the main pass and
// immediately before Present. See the implementation for why the cheaper
// alternatives are worse.
// ⚠️ IT TAKES THE VIEWS, NOT A SWAP CHAIN. In OVERLAY mode there is no
// `ISwapChain` at all -- the composition path wraps back buffers it did not
// create and owns a depth texture of its own -- so anything that reached for
// `GetCurrentBackBufferRTV` would work in the palette and be null on the overlay.
void DrawCornerGnomon (Diligent::IDeviceContext* context, DiligentScene& scene, Diligent::ITextureView* rtv,
                       Diligent::ITextureView* dsv, const Camera& camera, uint32_t width, uint32_t height);

// PLAT-RE65's anchors, once per frame: adopt a newly published set, then draw.
//
// ⚠️ THE GENERATION IS THE SIGNAL, NOT THE CONTENTS. Re-sending the same walls
// must still redraw them -- that is what "refresh the anchors after I moved a
// wall" means, and the guids would be identical. `lastSeq` is the caller's
// frame-loop local and is updated here.
//
// ⚠️ THE MUTEX IS HELD FOR THE SWAP ONLY, NEVER ACROSS THE UPLOAD. It is the
// same mutex the palette's state command takes, and holding it across a GPU map
// would block Archicad's main thread for as long as the driver takes.
// Record the pose the frame just PRESENTED was drawn with (PLAT-RE84).
//
// ⚠️ THIS IS HALF OF AN OBJECTIVE DESYNC NUMBER, and it is the half nothing else
// can supply. Every other stream says what ARCHICAD was doing; only the render
// thread knows what was actually on screen and when. `tools/navlog_report.py`
// interpolates Archicad's camera to each of these timestamps and reports the
// difference in PIXELS, plus the time-shift that best explains it -- which is
// the overlay's effective latency in milliseconds.
//
// ⚠️ CALL IT AFTER Present, NEVER BEFORE. Called at the top of the loop it would
// date every frame by however long that frame took to draw, which is a
// systematic error in exactly the quantity being measured.
//
// Plan path only -- it no-ops on a perspective camera, because the 3D path has
// no ground-truth stream to pair against yet. Also no-ops when the nav log is
// off, which it is by default.
// Copy the scene's per-frame numbers into the viewport's published stats. See
// the definition for why this lives here rather than in the frame body.
void CopySceneStatsInto (DiligentViewportStats& stats, const DiligentSceneStats& sceneStats);

void LogPresentedPlanFrame (const Camera& camera, uint32_t widthPx, uint32_t heightPx, uint64_t frameIndex);

// Register our swap chain with the DXGI Present hook and log its format.
// Render thread, once the target exists. See the .cpp for why the hook needs it.
void IdentifyOwnSwapChain (IDXGISwapChain* swapChain);

// Push the requested DXGI frame-latency onto the target, once per change.
// Render thread. `requested` of 0 leaves DXGI's default (3) in place, which is
// the baseline arm of the A/B.
// What a pick request was FOR, carried through DiligentPickBuffer's opaque tag.
// A click changes Archicad's selection; a hover only fills in the callout, and
// confusing the two makes moving the mouse rewrite the user's selection. Shared
// because the frame loop ISSUES picks and this file PUBLISHES them.
// At most one hover pick every N frames. ⚠️ IT SHARES ONE 8x8 TARGET WITH THE
// CLICK, so an unthrottled hover keeps a readback permanently in flight and every
// click waits behind one. Four frames is ~15 Hz at 60 fps, which is faster than a
// tooltip needs to feel live and leaves most frames free for a click.
//
// ⚠️ ONE DEFINITION, shared. It was declared identically in both this file's .cpp
// and DiligentViewport.cpp -- two copies of a tuning constant that only means
// anything if they agree.
constexpr uint64_t kHoverFramePeriod = 4;

constexpr uint32_t kPickTagClick = 1;
constexpr uint32_t kPickTagHover = 2;

// Compare the adopted camera generation against the newest published one, count
// it if superseded, then Present and publish the counters. Render thread.
// Mirror the frame just rendered into the surface Archicad's Present detour
// blits from (PLAT-RE79 phase 4). Render thread, AFTER the frame is drawn and
// BEFORE it is presented.
//
// ⚠️ IT IS A COPY, NOT A HANDOVER, AND THE OVERLAY PATH IS UNTOUCHED BY IT. The
// composition chain still renders and still presents exactly as it does with
// the hook off, so `mode=legacy` remains a live fallback rather than a code path
// somebody would have to restore -- which is the rule that made every rung of
// this ladder reversible. No-op outside overlay mode, and no-op when the host
// compositor is not armed.
// `camera` travels with the frame: an orthographic one publishes the plan pose
// it was drawn with, so the host can reproject it to the newest camera at blit
// time (PLAT-RE114). A perspective camera publishes an invalid pose and the host
// blits unwarped -- a flat image has no parallax to give.
void MirrorOverlayToHost (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                          DiligentViewportTarget& target, const Camera& camera);

void PresentAndAccount (DiligentViewportTarget& target, const std::atomic<uint64_t>& publishedGeneration,
                        uint64_t adoptedGeneration, std::atomic<uint64_t>& stalePresents,
                        std::atomic<uint64_t>& presentedGeneration, std::mutex& mutex, DiligentViewportStats& stats);

// Copy the live present counters into the stats. Render thread, every frame --
// see the .cpp for why "every frame" is the whole point.
void PublishPresentAccounting (DiligentViewportTarget& target, uint64_t stalePresents, std::mutex& mutex,
                               DiligentViewportStats& stats);

void ApplyRequestedFrameLatency (DiligentViewportTarget& target, uint32_t requested, uint32_t& applied);

// Publish a finished pick readback. Render thread; takes the viewport's mutex
// only for the moment it writes the stats.
void PublishCompletedPick (DiligentPickBuffer& pick, Diligent::IDeviceContext* context, uint64_t frames,
                           DiligentScene& scene, uint32_t& hoverId, std::mutex& mutex, DiligentViewportStats& stats);

// What a click and a hover have to REMEMBER between frames.
//
// ⚠️ IT IS A STRUCT BECAUSE THESE TEN VALUES ARE ONE MECHANISM, and they were
// ten separate locals in the render loop -- which is how `clickPending` came to
// be cleared on a path that had not issued a request. Passing them together
// means a new piece of pick state cannot be added to one half and forgotten in
// the other.
struct PickState {
    // The click. A DRAG IS NOT A CLICK -- without this, every orbit would also
    // select whatever the orbit started on.
    bool leftDown = false;
    bool leftDragged = false;
    int32_t leftDownX = 0;
    int32_t leftDownY = 0;
    // A click a previous frame could not issue because a readback was in flight.
    // Remembered rather than dropped; see ServicePick.
    bool clickPending = false;
    // The last hover the readback resolved, and the throttle keeping the hover
    // from monopolising the one pick target. 0 = the cursor is over nothing.
    uint32_t hoverId = 0;
    int32_t lastHoverX = -1;
    int32_t lastHoverY = -1;
    uint64_t nextHoverFrame = 0;
    // ⚠️ LATCHED, so a pick target that cannot be created does not write a line
    // per frame. Cleared on the first success, so a failure that ends reports
    // again if it returns.
    bool sizeReported = false;
};

// One frame of picking: turn this frame's input into a click or a throttled
// hover, render the id G-buffer, ask for the readback, and publish whatever the
// GPU has finished (PLAT-RE136). Render thread.
//
// ⚠️ IT REBINDS `rtv`/`dsv` ON THE WAY OUT and leaves the caller's targets as it
// found them, because the id pass has to unbind them to copy. The depth buffer
// is NOT re-cleared: the caller cleared it and the id pass used its own.
//
// ⚠️ `viewProj` MUST BE THE DISPLAYED IMAGE'S NOMINAL, UNJITTERED CAMERA. TAA's
// output resolves onto that grid even though its current geometry sample moves.
//
// `enabled` is the caller's "the model is actually on screen and the mouse is
// mine" test. ⚠️ IT STILL HAS TO BE CALLED WHEN FALSE: clearing the hover is an
// answer, and skipping the call leaves the last outline burned on the screen.
void ServicePick (DiligentPickBuffer& pick, PickState& state, bool enabled, Diligent::IRenderDevice* device,
                  Diligent::IDeviceContext* context, DiligentScene& scene, const InputSnapshot& input, uint64_t frames,
                  uint32_t width, uint32_t height, const float viewProj[16], Diligent::ITextureView* rtv,
                  Diligent::ITextureView* dsv, std::mutex& mutex, DiligentViewportStats& stats);

// The storey section overlay: turn the HUD's (or the capture's) settings into
// StorySliceLayer::DrawParams, draw the layer, and service the one-shot refresh
// request that switching it on raises.
//
// ⚠️ IT IS NOT GATED ON `offscreen`, unlike the plan anchors, and the caller must
// not add such a gate. A headless capture is the reason this layer exists on the
// Diligent side at all: a massing feasibility study renders one and needs the
// storey contours in it. The anchors' gate is about an instrument for checking
// register against a live Archicad window, which a capture does not have.
void UpdateAndDrawStorySlices (DiligentScene& scene, Diligent::IDeviceContext* context, HudState& hudState,
                               bool blanked, const float viewProj[16], uint32_t width, uint32_t height,
                               uint32_t colorFormat, uint32_t depthFormat);

void UpdateAndDrawPlanAnchors (PlanAnchorLayer& layer, Diligent::IRenderDevice* device,
                               Diligent::IDeviceContext* context, std::mutex& mutex,
                               std::vector<PlanAnchorVertex>& pending, uint64_t commandedSeq, uint64_t& lastSeq,
                               bool enabled, const float viewProj[16], uint32_t width, uint32_t height,
                               float widthPixels, uint32_t rgba);

} // namespace archviz
} // namespace geomsrv

#endif
