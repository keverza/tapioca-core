// ArchViz/DiligentViewportSupport — Diligent's diagnostic callback, the
// Archicad-camera conversion and the corner gnomon's pass. See the header for
// why these live apart from DiligentViewport.cpp's lifecycle.

#include "ArchViz/DiligentViewportSupport.hpp"

#include "ArchViz/Dxgi/HostComposite.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/Dxgi/SharedOverlaySurface.hpp"

#include "ArchViz/NavLog.hpp"   // the presented-frame half of the desync measurement

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentPickBuffer.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentShaders.hpp"   // DiligentDebugView, for the pass selector
#include "ArchViz/PlanAnchorLayer.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <atomic>
#include <mutex>

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <DebugOutput.h>   // SetDebugMessageCallback, DEBUG_MESSAGE_SEVERITY
#include <DeviceContext.h>
#include <DeviceContextD3D11.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDeviceD3D11.h>
#include <GraphicsTypes.h>
#include <TextureView.h>

#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

// Diligent's OWN diagnostics, into archviz.log.
//
// ⚠️ IT COST A ROUND TRIP NOT TO HAVE THIS. `CreateSwapChainD3D11 returned no
// swap chain` was all we could report, six times in a row, while Diligent itself
// had the HRESULT and the reason the whole time and was writing them to a
// callback nobody had installed. Every Diligent failure from here on says what
// Diligent thinks happened.
void DILIGENT_CALL_TYPE DiligentDebugMessage (Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                              const Diligent::Char* message,
                                              const Diligent::Char* function,
                                              const Diligent::Char* file, int line)
{
    const char* level = "info";
    switch (severity) {
        case Diligent::DEBUG_MESSAGE_SEVERITY_WARNING:     level = "WARNING"; break;
        case Diligent::DEBUG_MESSAGE_SEVERITY_ERROR:       level = "ERROR"; break;
        case Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR: level = "FATAL"; break;
        default: break;
    }
    std::string text = std::string ("Diligent [") + level + "] " +
                       (message != nullptr ? message : "(no message)");
    if (function != nullptr)
        text += std::string ("  in ") + function;
    if (file != nullptr)
        text += std::string ("  (") + file + ":" + std::to_string (line) + ")";
    ArchVizLog (text);
}

}   // namespace

void DrawSceneOrDebugView (DiligentScene& scene, Camera& camera, Diligent::IDeviceContext* context,
                           const SceneDrawRequest& request, int& lastLoggedGBufferView)
{
    const bool gBufferDebugView = request.debugView == int (DiligentDebugView::GBufferNormals) ||
                                  request.debugView == int (DiligentDebugView::GBufferDepth) ||
                                  request.debugView == int (DiligentDebugView::AmbientOcclusion) ||
                                  request.debugView == int (DiligentDebugView::GBufferAlbedo) ||
                                  request.debugView == int (DiligentDebugView::GBufferRoughness) ||
                                  request.debugView == int (DiligentDebugView::GBufferMaterialData) ||
                                  request.debugView == int (DiligentDebugView::MotionVectors);

    // ⚠️ CLEARED ON EVERY PATH THAT DOES NOT PREPARE IT. Draw multiplies in
    // whatever occlusion is standing, so a debug view -- or an AO pass that
    // failed -- must leave nothing behind, or the next ordinary frame darkens
    // with an older camera's occlusion and the shading appears to lag the orbit.
    scene.ClearAmbientOcclusion ();

    if (gBufferDebugView) {
        if (request.debugView != lastLoggedGBufferView) {
            lastLoggedGBufferView = request.debugView;
            ArchVizLog ("Diligent G-buffer view " + std::to_string (request.debugView) + ": near " +
                        std::to_string (Camera::NearClip ()) + " m, far " + std::to_string (camera.FarClip ()) +
                        " m, eye-to-target " + std::to_string (camera.Distance ()) + " m, " +
                        (camera.IsPerspective () ? "perspective" : "parallel"));
        }
        scene.DrawGBufferDebug (context, request.target, request.view, request.proj, request.viewProj, request.eye,
                                Camera::NearClip (), camera.FarClip (), camera.Distance (), camera.IsPerspective (),
                                request.frameIndex, CullMode::Cw, request.debugView);
        // ⚠️ ON THIS PATH TOO. See DiligentScene::AdvanceFrame: a frame that
        // skips the handover leaves the previous matrix two frames stale, and
        // the next motion vector comes out twice as long as the truth.
        scene.AdvanceFrame (request.motionViewProj);
        return;
    }

    // Re-arm the log, so switching away and back reports the camera as it is
    // THEN rather than staying silent.
    lastLoggedGBufferView = -1;

    // ⚠️ BEFORE Draw, AND IT IS A SECOND GEOMETRY PASS. See
    // DiligentScene::PrepareAmbientOcclusion: the forward path had never paid
    // for one until RE51.C3, and contact darkening cannot be had without depth
    // and normals for the whole frame. It is behind the HUD's own toggle, so
    // the cost is switchable rather than imposed.
    scene.SetAmbientOcclusion (request.ambientOcclusion, request.ambientOcclusionIntensity,
                               request.ambientOcclusionRadius);
    scene.SetScreenSpaceReflection (request.screenSpaceReflection, request.ssrIntensity,
                                     request.ssrRoughnessThreshold);
    scene.SetTemporalAntiAliasing (request.temporalAntiAliasing, request.taaStability);
    scene.PrepareAmbientOcclusion (context, request.view, request.proj, request.viewProj,
                                   request.motionViewProj, request.eye, request.jitter, Camera::NearClip (),
                                   camera.FarClip (), camera.Distance (), request.frameIndex, CullMode::Cw);
    scene.Draw (context, request.target, request.depth, request.view, request.proj, request.viewProj,
                request.motionViewProj, request.eye, request.jitter, CullMode::Cw, request.debugView,
                Camera::NearClip (), camera.FarClip (), camera.Distance (), request.frameIndex);
    scene.AdvanceFrame (request.motionViewProj);
}

void InstallDiligentDebugCallback ()
{
    Diligent::SetDebugMessageCallback (&DiligentDebugMessage);
}

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
bool ApplyArchicadCamera (Camera& camera, const CameraStart& start,
                          uint32_t width, uint32_t height, float* outDistance)
{
    if (!start.valid)
        return false;

    // ---- a 2D drawing window: parallel, straight down ----------------------
    // ⚠️ THE HEIGHT IS ARBITRARY AND THE ONLY THING IT AFFECTS IS CLIPPING. A
    // parallel projection's picture does not depend on how far away the eye is,
    // so this is chosen to clear any real project from above while keeping the
    // depth range small enough to be precise -- and Camera::GetProjMatrix takes
    // its far plane from it.
    if (start.orthographic) {
        constexpr float kPlanEyeHeightMetres = 5000.0f;
        camera.SetTarget (start.target[0], start.target[1], start.target[2]);
        camera.SetDistance (kPlanEyeHeightMetres);
        camera.SetTopDown (start.planRotationRadians);
        camera.SetOrthographic (true, start.orthoHalfHeightMetres);
        if (outDistance != nullptr)
            *outDistance = kPlanEyeHeightMetres;
        return true;
    }

    // ⚠️ AND OFF AGAIN ON THE 3D PATH, unconditionally. The overlay follows
    // whichever window is in front, so a user who switches from the floor plan
    // back to the 3D window pushes a perspective camera into a camera that is
    // still parallel -- which draws the model at plan scale from a 3D pose and
    // reads as the overlay having broken.
    camera.SetOrthographic (false, 0.0f);

    const float dx = start.eye[0] - start.target[0];
    const float dy = start.eye[1] - start.target[1];
    const float dz = start.eye[2] - start.target[2];
    const float distance = std::sqrt (dx * dx + dy * dy + dz * dz);
    if (outDistance != nullptr)
        *outDistance = distance;

    camera.SetTarget (start.target[0], start.target[1], start.target[2]);
    if (distance > 1e-4f) {
        camera.SetDistance (distance);
        camera.SetOrbit (std::atan2 (dy, dx), std::asin (dz / distance));
    }

    // ⚠️ ARCHICAD'S viewCone IS HORIZONTAL AND IN DEGREES; the camera's field of
    // view is VERTICAL. Converting needs the aspect ratio, which is why it
    // happens here and not in the palette: fovV = 2*atan(tan(fovH/2) / aspect).
    if (start.viewConeDegreesHorizontal > 1.0f && height > 0) {
        constexpr float kPi = 3.14159265358979323846f;
        const float aspect = float (width) / float (height);
        const float halfH = start.viewConeDegreesHorizontal * 0.5f * (kPi / 180.0f);
        const float halfV = std::atan (std::tan (halfH) / (aspect > 0.0f ? aspect : 1.0f));
        camera.SetFovDegreesVertical (halfV * 2.0f * (180.0f / kPi));
    }
    return true;
}

namespace {

// The axis gnomon's corner, in screen pixels.
constexpr int kGnomonBoxPixels = 110;
constexpr int kGnomonMarginPixels = 12;

}   // namespace

// The gnomon, in a small square viewport in the bottom-left corner.
//
// ⚠️ IT BORROWS ONLY THE CAMERA'S *ORIENTATION*, not its position or its
// distance. The gnomon must turn exactly as the model turns and must not change
// size when the user zooms; slaving it to the whole camera would put it
// kilometres away on a site model and inside the arrows on a detail. So: a fixed
// eye at a fixed distance, orbited by the scene camera's own yaw and pitch.
void DrawCornerGnomon (Diligent::IDeviceContext* context, DiligentScene& scene,
                       Diligent::ITextureView* rtv, Diligent::ITextureView* dsv,
                       const Camera& camera, uint32_t width, uint32_t height)
{
    if (context == nullptr || rtv == nullptr)
        return;
    const int box = kGnomonBoxPixels;
    if (width < uint32_t (box + kGnomonMarginPixels) ||
        height < uint32_t (box + kGnomonMarginPixels))
        return;   // the palette is too small to spare the corner

    Camera gnomonCamera;
    gnomonCamera.SetTarget (0.0f, 0.0f, 0.0f);
    gnomonCamera.SetDistance (6.5f);
    gnomonCamera.SetFovDegreesVertical (35.0f);
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    gnomonCamera.SetOrbit (camera.YawDegrees () * kDegToRad,
                           camera.PitchDegrees () * kDegToRad);

    float view[16];
    float proj[16];
    float viewProj[16];
    gnomonCamera.GetViewMatrix (view);
    gnomonCamera.GetProjMatrix (proj, 1.0f);   // the viewport is square
    Multiply (viewProj, view, proj);

    // ⚠️ THE TARGETS ARE BOUND FIRST AND THE VIEWPORT SET AFTER, AND THE ORDER
    // IS THE WHOLE CORNER. `SetRenderTargets` RESETS the viewport to the full
    // render target whenever the framebuffer actually CHANGES -- see
    // DeviceContextD3D11Impl::SetRenderTargetsExt, which calls
    // SetViewports (1, nullptr, 0, 0) inside the `if (framebuffer changed)`.
    // Setting the 110-pixel box first therefore survives only when the binding
    // is a no-op, which is exactly what the Final path does and exactly what
    // the G-buffer debug views do NOT: DrawGBufferDebug leaves the colour
    // target bound with a NULL depth view, so re-attaching `dsv` here is a real
    // change and the box was thrown away. The symptom is not a missing gnomon
    // but a full-screen one -- its camera sits 6.5 m from the origin, so the
    // arrows fill the whole viewport and read as world geometry.
    context->SetRenderTargets (1, &rtv, dsv,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    Diligent::Viewport viewport;
    viewport.TopLeftX = float (kGnomonMarginPixels);
    viewport.TopLeftY = float (int (height) - box - kGnomonMarginPixels);
    viewport.Width = float (box);
    viewport.Height = float (box);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->SetViewports (1, &viewport, width, height);

    // ⚠️ THE WHOLE DEPTH BUFFER IS CLEARED, AND THAT IS DELIBERATE. Without it
    // the gnomon is depth-tested against the building and vanishes behind
    // whatever wall happens to occupy the bottom-left of the screen, which is
    // the ordinary case rather than a corner case. The alternatives are worse:
    // unbinding the depth view draws a depth-enabled pipeline with no DSV (a
    // validation error, not a picture), and a scissored partial clear needs
    // ScissorEnable on a rasterizer state the mesh pipelines do not have. This
    // runs AFTER the main pass and immediately before Present, so the cleared
    // depth is never read again -- it costs one clear and nothing else.
    if (dsv != nullptr)
        context->ClearDepthStencil (dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    float eye[3];
    gnomonCamera.GetEyePosition (eye);
    scene.DrawOverlay (context, viewProj, eye);

    // Give the caller back the full-surface viewport, so a later pass does not
    // inherit a 110-pixel window.
    context->SetViewports (1, nullptr, 0, 0);
}

void UpdateAndDrawPlanAnchors (PlanAnchorLayer& layer, Diligent::IRenderDevice* device,
                               Diligent::IDeviceContext* context,
                               std::mutex& mutex, std::vector<PlanAnchorVertex>& pending,
                               uint64_t commandedSeq, uint64_t& lastSeq,
                               bool enabled, const float viewProj[16],
                               uint32_t width, uint32_t height,
                               float widthPixels, uint32_t rgba)
{
    if (commandedSeq != lastSeq) {
        lastSeq = commandedSeq;

        std::vector<PlanAnchorVertex> ribbon;
        {
            std::lock_guard<std::mutex> lock (mutex);
            ribbon.swap (pending);
        }
        std::string uploadError;
        if (!layer.Upload (device, context, ribbon, uploadError))
            ArchVizLog ("Diligent viewport: plan anchors not uploaded (" + uploadError + ")");
    }

    if (enabled)
        layer.Draw (context, viewProj, width, height, widthPixels, rgba);
}

void LogPresentedPlanFrame (const Camera& camera, uint32_t widthPx, uint32_t heightPx,
                            uint64_t frameIndex)
{
    if (!navlog::IsRunning ())
        return;

    float cameraTarget[3] = {0.0f, 0.0f, 0.0f};
    camera.GetTarget (cameraTarget);

    // ⚠️ THE 3D HALF USED TO BE A BARE `return`, which is why no `path=3d` run
    // has ever produced a comparable pair. A perspective camera is logged in
    // ITS terms rather than skipped.
    if (!camera.IsOrthographic ()) {
        float eye[3] = {0.0f, 0.0f, 0.0f};
        camera.GetEyePosition (eye);
        // Archicad reports a HORIZONTAL cone; the camera holds a vertical FOV.
        // Converting here keeps both streams in one convention -- see NavLog.hpp.
        const double aspect = heightPx > 0 ? double (widthPx) / double (heightPx) : 1.0;
        const double fovVerticalRad = double (camera.FovDegreesVertical ()) * 3.14159265358979323846 / 180.0;
        const double fovHorizontalDeg =
            2.0 * std::atan (std::tan (fovVerticalRad * 0.5) * aspect) * 180.0 / 3.14159265358979323846;
        const double eyeD[3] = { double (eye[0]), double (eye[1]), double (eye[2]) };
        const double targetD[3] = { double (cameraTarget[0]), double (cameraTarget[1]),
                                    double (cameraTarget[2]) };
        navlog::LogViewerPersp (eyeD, targetD, fovHorizontalDeg, widthPx, heightPx, frameIndex);
        return;
    }
    const double centre[2] = { double (cameraTarget[0]), double (cameraTarget[1]) };
    navlog::LogViewerPlan (centre, double (camera.OrthoHalfHeightMetres ()),
                           double (camera.TopDownRotationRadians ()), widthPx, heightPx,
                           frameIndex);
}

// A pick's readback, once the GPU has finished with it.
//
// ⚠️ POLLED, NEVER WAITED FOR AT THE POINT OF THE CLICK. The GPU is pipelined, so
// the copy the click issued is not finished in that frame; reading it there
// would return the PREVIOUS pick -- every click selecting whatever the last one
// was over.
//
// ⚠️ ONLY A CLICK PUBLISHES. A hover that bumped `pickSeq` would make the
// selection bridge re-select whatever the mouse passed over, so simply moving
// the cursor across the viewport would rewrite Archicad's selection.
void PublishCompletedPick (DiligentPickBuffer& pick, Diligent::IDeviceContext* context,
                           uint64_t frames, DiligentScene& scene, uint32_t& hoverId,
                           std::mutex& mutex, DiligentViewportStats& stats)
{
    uint32_t pickedId = 0;
    uint32_t pickTag = kPickTagHover;
    if (!pick.Poll (context, frames, pickedId, pickTag))
        return;
    hoverId = pickedId;
    if (pickTag != kPickTagClick)
        return;
    const std::string picked = scene.GuidForId (pickedId);

    // ⚠️ SELECT LOCALLY, HERE, RATHER THAN WAITING FOR ARCHICAD TO SAY SO.
    //
    // The highlight used to arrive only by round trip: the click published
    // `pickedGuid`, SelectionBridge's main-thread timer selected it in ARCHICAD,
    // and Archicad's selection came back to tint the mesh. That works only while
    // the viewer is allowed to WRITE to Archicad's selection -- and the panel is
    // deliberately inspection-only now (selectionbridge::ToViewer), so with the
    // write half off the round trip has no return leg and a click highlighted
    // nothing at all. Selecting here makes the viewer's own selection a fact of
    // the viewer, which is what an inspection tool needs and what removes the
    // one-timer-tick delay the round trip cost even when it did work.
    //
    // ⚠️ IT DOES NOT REPLACE THE BRIDGE. `ToViewer` still applies ARCHICAD's
    // selection over the top; whichever happened most recently is what shows,
    // which is the correct answer for both directions.
    //
    // An empty guid is the sky, and clearing the selection is what clicking the
    // sky should do.
    scene.SetSelection (picked.empty () ? std::vector<std::string> {}
                                        : std::vector<std::string> {picked});

    std::lock_guard<std::mutex> lock (mutex);
    stats.pickedGuid = picked;   // empty = the sky, i.e. deselect
    ++stats.pickSeq;
}

void ServicePick (DiligentPickBuffer& pick, PickState& state, bool enabled,
                  Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                  DiligentScene& scene, const InputSnapshot& input, uint64_t frames,
                  uint32_t width, uint32_t height, const float viewProj[16],
                  Diligent::ITextureView* rtv, Diligent::ITextureView* dsv,
                  std::mutex& mutex, DiligentViewportStats& stats)
{
    if (!pick.IsReady () || !enabled) {
        // Picking did not run this frame -- it is unavailable, the cursor is over
        // the HUD, or the model is not being drawn at all. ⚠️ THE HOVER IS STILL
        // CLEARED AND STILL PUSHED: over the HUD the user is not pointing at the
        // model, and a stale outline otherwise outlives the thing that drew it.
        state.hoverId = 0;
        scene.SetHoverId (0);
        return;
    }

    bool clicked = state.clickPending;   // a click an earlier frame could not issue
    // ⚠️ THE TRANSITIONS, NOT `buttons`. A press and release inside one 16 ms
    // frame is an ordinary fast click and collapses to "nothing happened" in the
    // held-state byte -- which is exactly the click a user makes when they mean
    // to select something.
    for (int t = 0; t < input.transitionCount; ++t) {
        const ButtonTransition& transition = input.transitions[t];
        if (transition.button != kMouseLeft)
            continue;
        if (transition.down) {
            state.leftDown = true;
            state.leftDragged = false;
            // The polled cursor for THIS frame, not the position at the instant
            // of the press -- DG delivers the transition without one. Same frame,
            // so at most one frame of mouse travel, which the readback box
            // absorbs.
            state.leftDownX = input.x;
            state.leftDownY = input.y;
        } else if (state.leftDown) {
            state.leftDown = false;
            if (!state.leftDragged && input.inside)
                clicked = true;
        }
    }
    // A few pixels of slack: a click is never perfectly still, and treating any
    // movement as a drag makes picking feel broken.
    if (state.leftDown && (std::abs (input.x - state.leftDownX) > 3 ||
                           std::abs (input.y - state.leftDownY) > 3))
        state.leftDragged = true;

    // ⚠️ A CLICK THAT CANNOT BE ISSUED IS REMEMBERED, NOT DROPPED, AND THAT IS A
    // FIX. The old code was `if (clicked && !HasPendingRequest())` -- so a click
    // landing while any readback was in flight simply vanished, with nothing
    // anywhere saying so. That is the reported "sometimes does not register
    // selection", and the hover requests below would have made it the common case
    // rather than a rare one.
    state.clickPending = clicked && pick.HasPendingRequest ();

    // ---- hover (PLAT-RE43, PLAT-RE136) -------------------------------------
    // ⚠️ A CLICK ALWAYS WINS, AND THE HOVER IS THROTTLED BEHIND IT. Both are the
    // same pass over the same target, so issuing a hover every frame would put a
    // readback permanently in flight and every click would queue behind one. Only
    // re-issuing when the cursor has actually MOVED also means a still mouse
    // costs nothing at all.
    //
    // ⚠️ NO LONGER GATED ON THE CALLOUT. The hover now drives the outline that
    // says what a click would take, which is wanted whether or not the user has
    // the properties callout open -- gating it on `showCallout` made the
    // highlight appear to depend on an unrelated HUD toggle.
    //
    // ⚠️ MOVEMENT ALONE, NOT `|| hoverId == 0`, AND THE ID G-BUFFER IS WHY. That
    // clause re-asked whenever the last answer was "nothing" -- so a cursor
    // resting anywhere over the background re-issued a pick forever, every
    // `kHoverFramePeriod` frames, because the answer never changed. Against the
    // old 8x8 target that was invisible; against a full-resolution id pass it is
    // a clear of an RGBA8 + D32 surface and a rasterisation of the whole model,
    // ~15 times a second, for a question already answered.
    //
    // What it bought: an outline appearing under a STATIONARY cursor when
    // geometry streams in underneath it. `lastHoverX` starts at -1 so the first
    // hover of a session still fires; after that the user moves the mouse a
    // pixel and gets it. That is the cheaper side of the trade.
    const bool cursorMoved = std::abs (input.x - state.lastHoverX) > 1 ||
                             std::abs (input.y - state.lastHoverY) > 1;
    const bool wantHover = input.inside && !clicked && cursorMoved &&
                           frames >= state.nextHoverFrame;

    // ⚠️ THE CURSOR LEAVING IS AN ANSWER, AND IT NEEDS NO GPU. No hover pick is
    // issued from outside the viewport, so without this the last id stays and the
    // outline is left burned onto whatever the mouse was over when it wandered
    // off the panel.
    if (!input.inside)
        state.hoverId = 0;

    if ((clicked || wantHover) && !pick.HasPendingRequest ()) {
        // The CLICK aims at where the button went down; a HOVER aims at where the
        // cursor is now. Aiming a click at the current cursor would let the few
        // pixels the hand travels between press and release change what gets
        // selected.
        const int32_t aimX = clicked ? state.leftDownX : input.x;
        const int32_t aimY = clicked ? state.leftDownY : input.y;

        // ⚠️ BEFORE Begin, NEVER BETWEEN Begin AND Request: it releases the
        // textures the context would be holding. A failure here costs picking for
        // the frame and nothing else, so it is reported once and the frame
        // carries on.
        std::string sizeError;
        if (!pick.EnsureSize (device, width, height, sizeError)) {
            if (!state.sizeReported) {
                ArchVizLog ("Diligent viewport: the pick id target could not be sized (" +
                            sizeError + "); picking is off until the next resize");
                state.sizeReported = true;
            }
        } else {
            state.sizeReported = false;
            pick.Begin (context);
            // ⚠️ THE FRAME'S OWN viewProj, THE ONE scene.Draw IS GIVEN -- that
            // identity is the whole of PLAT-RE136. And ⚠️ THE SAME CULL MODE: a
            // pick pass that culls differently resolves a click to a face the
            // user cannot see.
            scene.DrawIds (context, viewProj, CullMode::Cw);
            // Unbind, THEN copy: D3D11 will not copy a resource that is still
            // bound for output, and it says so in a validation message rather
            // than by failing.
            pick.End (context);
            if (pick.Request (context, aimX, aimY, frames,
                              clicked ? kPickTagClick : kPickTagHover)) {
                if (!clicked) {
                    state.lastHoverX = input.x;
                    state.lastHoverY = input.y;
                    state.nextHoverFrame = frames + kHoverFramePeriod;
                }
                // ⚠️ ONLY ON A REQUEST THAT WAS ACTUALLY ISSUED. A click whose
                // Request was refused -- the cursor outside the target -- must
                // stay remembered, or it is the dropped click of PLAT-RE136 again
                // by a different route.
                state.clickPending = false;
            }
            // ⚠️ PUT THE FRAME'S TARGETS BACK. Begin/End bound and then unbound
            // the id target; without this the scene draws into nothing and the
            // viewport shows only the clear colour on every frame the user
            // clicks. The depth buffer is NOT re-cleared -- the caller cleared it
            // and the pick pass used its own.
            context->SetRenderTargets (1, &rtv, dsv,
                                       Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    PublishCompletedPick (pick, context, frames, scene, state.hoverId, mutex, stats);

    // What the hover outline follows. ⚠️ EVERY FRAME, INCLUDING THE ONES THAT
    // RESOLVED TO NOTHING -- the scene needs to be told the hover ENDED just as
    // much as it needs to be told it started, and only pushing non-zero ids
    // leaves the last outline on screen forever.
    scene.SetHoverId (state.hoverId);
}

// The live present counters, copied into the stats every frame.
//
// ⚠️ EVERY FRAME, NOT FROM THE CLEAR A/B BLOCK. They were published from there,
// which runs ONCE and is skipped entirely in overlay mode -- so `stalePresents`
// and the effective frame latency read zero for exactly the runs they were added
// to measure, and a zero there is indistinguishable from "nothing went stale".
// The A/B block is for one-shot device facts; these are counters.
//
// ⚠️ `frameLatency` IS WHAT WAS APPLIED, NOT WHAT WAS ASKED FOR. Setting it is
// asynchronous -- the request is an atomic the render thread picks up later --
// so a probe that echoed its own parameter would report success before anything
// had happened. This is the acknowledgement.
void PublishPresentAccounting (DiligentViewportTarget& target, uint64_t stalePresents,
                               std::mutex& mutex, DiligentViewportStats& stats);

void MirrorOverlayToHost (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                          DiligentViewportTarget& target, const Camera& camera)
{
    if (!dxgi::HostCompositeEnabled () || target.Mode () != SurfaceMode::Overlay)
        return;
    IDXGISwapChain* chain = target.Dxgi ();
    if (device == nullptr || context == nullptr || chain == nullptr)
        return;

    Diligent::RefCntAutoPtr<Diligent::IRenderDeviceD3D11> nativeDevice {
        device, Diligent::IID_RenderDeviceD3D11};
    Diligent::RefCntAutoPtr<Diligent::IDeviceContextD3D11> nativeContext {
        context, Diligent::IID_DeviceContextD3D11};
    if (!nativeDevice || !nativeContext)
        return;

    std::string error;
    if (!dxgi::EnsureSharedOverlaySurface (nativeDevice->GetD3D11Device (), target.Width (),
                                           target.Height (), error)) {
        // Once per change of reason, not once per frame: this runs at 60 Hz and
        // a repeated line would bury the log it shares with everything else.
        static std::string lastError;
        if (error != lastError) {
            lastError = error;
            ArchVizLog ("host composite mirror: " + error);
        }
        return;
    }

    // ⚠️ THE COMPOSITION BACK BUFFER, FETCHED FRESH. It is a flip-model chain,
    // so buffer 0 is a different surface after every Present and a cached
    // pointer would copy the frame BEFORE the one on screen -- a permanent extra
    // frame of lag introduced by the very step meant to remove one.
    // The pose this frame was DRAWN with, published with the pixels -- see
    // PublishSharedOverlayFrame for why the two travel together.
    dxgi::SharedOverlayPose pose;
    if (camera.IsOrthographic () && camera.OrthoHalfHeightMetres () > 0.0f) {
        float target3[3] = {};
        camera.GetTarget (target3);
        pose.valid = true;
        pose.centreX = target3[0];
        pose.centreY = target3[1];
        pose.halfHeightMetres = camera.OrthoHalfHeightMetres ();
        pose.rotationRadians = camera.TopDownRotationRadians ();
    }

    ID3D11Texture2D* frame = nullptr;
    if (SUCCEEDED (chain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &frame)) &&
        frame != nullptr) {
        dxgi::PublishSharedOverlayFrame (nativeContext->GetD3D11DeviceContext (), frame, pose);
        frame->Release ();
    }
}

void PresentAndAccount (DiligentViewportTarget& target,
                        const std::atomic<uint64_t>& publishedGeneration,
                        uint64_t adoptedGeneration, std::atomic<uint64_t>& stalePresents,
                        std::atomic<uint64_t>& presentedGeneration, std::mutex& mutex,
                        DiligentViewportStats& stats)
{
    // ⚠️ CHECKED IMMEDIATELY BEFORE Present, WHICH IS THE ONLY HONEST PLACE. A
    // camera that arrived after this frame adopted one is already too late for
    // it -- the frame goes out carrying the older pose. Counting that is the
    // difference between "the overlay is 15 ms behind" and "the overlay is
    // showing a camera it knew was superseded before it was even submitted".
    if (publishedGeneration.load (std::memory_order_acquire) > adoptedGeneration)
        stalePresents.fetch_add (1, std::memory_order_relaxed);
    presentedGeneration.store (adoptedGeneration, std::memory_order_release);
    target.Present ();
    PublishPresentAccounting (target, stalePresents.load (std::memory_order_relaxed),
                              mutex, stats);
}

void PublishPresentAccounting (DiligentViewportTarget& target, uint64_t stalePresents,
                               std::mutex& mutex, DiligentViewportStats& stats)
{
    std::lock_guard<std::mutex> lock (mutex);
    stats.stalePresents = stalePresents;
    stats.presentFailures = target.PresentFailures ();
    stats.lastPresentResult = target.LastPresentResult ();
    stats.frameLatency = target.FrameLatency ();
}

void ApplyRequestedFrameLatency (DiligentViewportTarget& target, uint32_t requested,
                                 uint32_t& applied)
{
    // ⚠️ 0 DOES NOT MEAN "RESTORE THE DEFAULT", AND PRETENDING IT DID BROKE THE
    // WHOLE A/B. There is no DXGI call that un-sets a frame latency, so the old
    // `requested == 0 -> return` left the previously applied value in place --
    // and since the viewport defaults to 1, the "baseline" arm measured 1 as
    // well. Two arms, one configuration, and the comparison would have read as
    // "frame latency changes nothing".
    //
    // The baseline is now applied EXPLICITLY: 3 is DXGI's own default depth, so
    // asking for 3 reproduces it rather than hoping. 0 is rejected upstream.
    if (requested == applied || requested == 0)
        return;
    std::string error;
    if (target.SetFrameLatency (requested, error)) {
        applied = requested;
        ArchVizLog ("overlay frame latency set to " + std::to_string (requested) +
                    " (DXGI's default is 3, i.e. up to three finished frames may wait)");
    } else {
        // ⚠️ RECORDED AS APPLIED EVEN ON FAILURE. Otherwise this retries on every
        // frame forever, logging every time, on a driver that will never support
        // it -- turning a missing optimisation into a flood.
        applied = requested;
        ArchVizLog ("overlay frame latency NOT set: " + error);
    }
}

void IdentifyOwnSwapChain (IDXGISwapChain* swapChain)
{
    // ⚠️ TELL THE PRESENT HOOK WHICH CHAIN IS OURS. One vtable serves every swap
    // chain in the process, so the hook sees Archicad's frames and the overlay's
    // interleaved. Without a label the frame-clock table has to be read by
    // inference -- and the first hookdiag run produced one steady 59 Hz chain and
    // one irregular 50 Hz chain, where guessing wrong reverses the conclusion
    // about whether there is a cadence to hit at all.
    dxgi::SetOwnSwapChain (uint64_t (uintptr_t (swapChain)));

    DXGI_SWAP_CHAIN_DESC nativeDesc {};
    if (swapChain != nullptr && SUCCEEDED (swapChain->GetDesc (&nativeDesc)))
        ArchVizLog ("Diligent viewport DXGI: format=" +
                    std::to_string ((uint32_t) nativeDesc.BufferDesc.Format) + " effect=" +
                    std::to_string ((uint32_t) nativeDesc.SwapEffect) + " buffers=" +
                    std::to_string (nativeDesc.BufferCount) + " windowed=" +
                    std::to_string (nativeDesc.Windowed != FALSE));
}

}   // namespace archviz
}   // namespace geomsrv
