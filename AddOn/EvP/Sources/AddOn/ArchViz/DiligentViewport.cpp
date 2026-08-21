// The viewport's RENDER THREAD: own the device, swap chain and scene, run the
// frame loop, tear it down in the order the flip model demands. That is all this
// file does.
//
// The main thread's half -- the singleton, Start/Stop/RequestResize, and the
// publishers that hand this loop a new camera, sun or anchor set -- is in
// ArchViz/DiligentViewportControl.cpp. The split is on the THREAD BOUNDARY, not
// on size, so which file a function belongs in is decided by what it touches
// rather than by how full each one is.

#include "ArchViz/DiligentViewport.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Camera.hpp"
#include "ArchViz/DebugCubeMesh.hpp"
#include "ArchViz/DiligentClearAB.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentPickBuffer.hpp"
#include "ArchViz/AxisGnomonMesh.hpp"
#include "ArchViz/DiligentCameraRays.hpp"
#include "ArchViz/DiligentGpuTimings.hpp"
#include "ArchViz/PlanAnchorLayer.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentShaders.hpp"
#include "ArchViz/DiligentViewportSupport.hpp"
#include "ArchViz/DiligentViewportTarget.hpp"
#include "ArchViz/HardwareInput.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/Uniforms.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede Diligent's D3D11 interop header (Probe 1a).
#include <EngineFactoryD3D11.h>
#include <DeviceContextD3D11.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDeviceD3D11.h>
#include <SwapChainD3D11.h>
#include <TextureViewD3D11.h>

#include <chrono>
#include <cmath>
#include <cstdlib> // std::abs for the click-vs-drag slack, on int32_t
#include <exception>
#include <stdexcept>
#include <vector>

namespace geomsrv::archviz {

void DiligentViewport::Run (Surface surface, CameraStart cameraStart)
{
    // ⚠️ DECLARED OUTSIDE THE try SO THE FAILURE PATH CAN RELEASE THEM PROPERLY.
    // An exception thrown after the swap chain exists -- a shader that does not
    // compile, a pipeline state that will not create -- would otherwise unwind
    // these without the ClearState the flip model requires, and leave the HWND
    // unusable for the rest of the Archicad session exactly as a clean close
    // used to.
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    // WHERE the frames go: the DG palette child's HWND swap chain, or the
    // DirectComposition overlay over Archicad's own 3D view. One frame loop
    // serves both -- see DiligentViewportTarget.hpp for why that is not
    // optional.
    DiligentViewportTarget target;
    // Borrowed from the target once it exists, for the per-frame present count.
    // Never released -- the target owns it.
    IDXGISwapChain* dxgiSwapChain = nullptr;
    uint32_t appliedLatency = 0;    // last value pushed, so a re-apply is a no-op
    uint64_t adoptedGeneration = 0; // which published camera this frame carries
    DiligentScene scene;
    PlanAnchorLayer planAnchors;
    DiligentHud hud;
    DiligentPickBuffer pick;
    DiligentGpuTimings gpuTimings;

    // Give back the HWND. See the long note at the call site in the success
    // path; this is that same teardown, reachable from both.
    auto releaseEverything = [&] () {
        gpuTimings.Collect ();
        gpuTimings.Shutdown ();
        scene.Shutdown ();
        hud.Shutdown ();
        pick.Shutdown ();
        dxgiSwapChain = nullptr;
        IdentifyOwnSwapChain (nullptr); // clears the Present hook's label
        // ⚠️ THE TARGET GOES BEFORE THE CONTEXT AND THE DEVICE, and it is handed
        // the context so it can unbind first. PLAT-RE39 is the standing proof of
        // what happens otherwise: the flip model defers the swap chain's
        // destruction while the immediate context still holds its back buffers,
        // the HWND stays taken, and every reopen fails E_ACCESSDENIED for the
        // rest of the Archicad session.
        target.Destroy (context);
        if (context != nullptr) {
            Diligent::RefCntAutoPtr<Diligent::IDeviceContextD3D11> nativeContext { context,
                                                                                   Diligent::IID_DeviceContextD3D11 };
            if (nativeContext != nullptr) {
                if (ID3D11DeviceContext* d3dContext = nativeContext->GetD3D11DeviceContext ()) {
                    // Diligent's InvalidateState only drops its OWN state cache;
                    // SetRenderTargets(0,...) unbinds render targets but not the
                    // shader resources, samplers and vertex buffers that also
                    // hold references. ClearState is the documented one.
                    d3dContext->ClearState ();
                    d3dContext->Flush ();
                }
            }
            else {
                ArchVizLog ("Diligent viewport: could not reach the native D3D11 context to "
                            "clear it -- reopening the viewport may fail with 'no swap chain'");
            }
        }
        context.Release ();
        device.Release ();
    };

    try {
        // Diligent's own diagnostics, into archviz.log. Installed before the
        // first Diligent call so a device or swap-chain failure explains itself.
        InstallDiligentDebugCallback ();

        Diligent::EngineD3D11CreateInfo engineCI;
        Diligent::IEngineFactoryD3D11* factory = Diligent::GetEngineFactoryD3D11 ();
        factory->CreateDeviceAndContextsD3D11 (engineCI, &device, &context);
        if (device == nullptr || context == nullptr)
            throw std::runtime_error ("Diligent returned no D3D11 device or immediate context");

        if (gpuTimings.Initialize (device))
            ArchVizLog ("Diligent benchmark GPU timings enabled");

        std::string targetError;
        if (!target.Create (device, context, factory, surface.mode, surface.nwh, surface.width, surface.height,
                            targetError))
            throw std::runtime_error (targetError);

        dxgiSwapChain = target.Dxgi ();
        IdentifyOwnSwapChain (dxgiSwapChain);

        // Render thread, because the swap chain only exists here. The why is in
        // DiligentViewportSupport's ApplyRequestedFrameLatency.
        ApplyRequestedFrameLatency (target, requestedFrameLatency_.load (), appliedLatency);

        {
            std::lock_guard<std::mutex> lock (mutex_);
            stats_.initialized = true;
            stats_.overlay = surface.mode == SurfaceMode::Overlay;
        }
        ArchVizLog (std::string ("Diligent viewport: device, immediate context and ") +
                    (surface.mode == SurfaceMode::Overlay ? "COMPOSITION OVERLAY surface" : "HWND swap chain") +
                    " initialized");

        // ---- the scene ------------------------------------------------------
        // The real one: SceneCmdQueue's elements, with Archicad's own materials
        // and sun. The helper meshes (the axis gnomon, and the debug cube until
        // geometry arrives) are static meshes beside them.
        HudState hudState;
        Camera camera;
        {
            std::string sceneError;
            if (!scene.Init (device, target.ColorFormat (), target.DepthFormat (), sceneError))
                throw std::runtime_error (sceneError);

            // ⚠️ NO SKY BEHIND THE MODEL ON THE COMPOSITION OVERLAY. There the
            // viewport is a transparent layer over Archicad's own 3D window,
            // and an opaque background would hide the very thing the overlay
            // exists to annotate -- the same reason the HUD goes read-only in
            // that mode a few lines below.
            scene.SetEnvironmentBackground (surface.mode != SurfaceMode::Overlay);

            // ⚠️ THE GNOMON IS NOT DECORATION. A mirrored image is the one
            // rendering fault that otherwise looks perfectly fine. Red east,
            // green north, blue up, always on screen.
            //
            // ⚠️ AND IT IS AN *OVERLAY*, NOT A WORLD MESH -- that was found the
            // expensive way. As a two-metre object at the project origin it was
            // simply invisible in the first live run against a real model,
            // because the project origin is inside the ground floor slab. It now
            // lives in a fixed corner of the screen at a fixed size, which is
            // also what every DCC does and for the same reason.
            std::vector<ArchVizVertex> gnomonVertices;
            std::vector<uint16_t> gnomonIndices;
            axisgnomon::Build (gnomonVertices, gnomonIndices);
            if (!scene.AddOverlayMesh (device, "axis gnomon", gnomonVertices.data (), gnomonVertices.size (),
                                       gnomonIndices.data (), gnomonIndices.size (), sceneError))
                ArchVizLog ("Diligent viewport: the axis gnomon did not upload: " + sceneError);

            // The cube stays until Archicad's geometry lands, so an empty
            // project and a broken extraction do not look the same. NEUTRAL
            // now: the gnomon carries orientation, so the cube can answer the
            // shading question instead of competing with it.
            ArchVizVertex cubeVertices[debugcubemesh::kVertexCount];
            uint16_t cubeIndices[debugcubemesh::kIndexCount];
            debugcubemesh::Build (cubeVertices, cubeIndices, debugcubemesh::Palette::Neutral);
            if (!scene.AddStaticMesh (device, "debug cube", cubeVertices, debugcubemesh::kVertexCount, cubeIndices,
                                      debugcubemesh::kIndexCount, sceneError))
                ArchVizLog ("Diligent viewport: the debug cube did not upload: " + sceneError);

            // ---- where the camera starts ------------------------------------
            float distance = 0.0f;
            if (ApplyArchicadCamera (camera, cameraStart, surface.width, surface.height, &distance)) {
                ArchVizLog ("Diligent viewport camera from Archicad (" + cameraStart.source + "): target " +
                            std::to_string (cameraStart.target[0]) + "," + std::to_string (cameraStart.target[1]) +
                            "," + std::to_string (cameraStart.target[2]) + " distance " + std::to_string (distance) +
                            " viewCone(H) " + std::to_string (cameraStart.viewConeDegreesHorizontal));
            }
            else {
                const float half = debugcubemesh::kHalfExtent;
                const float boundsMin[3] = { -half, -half, -half };
                const float boundsMax[3] = { half, half, half };
                camera.FrameBounds (boundsMin, boundsMax);
                ArchVizLog ("Diligent viewport camera: no Archicad camera (" + cameraStart.source +
                            "); framing the debug cube instead");
            }

            const DiligentSceneStats sceneStats = scene.Stats ();
            {
                std::lock_guard<std::mutex> lock (mutex_);
                stats_.sceneReady = scene.IsReady ();
                stats_.sceneTriangles = sceneStats.triangles;
                stats_.sceneVertices = sceneStats.vertices;
                stats_.sceneGpuBytes = sceneStats.gpuBytes;
            }
            ArchVizLog ("Diligent viewport scene ready: " + std::to_string (sceneStats.triangles) +
                        " helper triangles, waiting for "
                        "Archicad geometry");

            // ⚠️ THE HUD'S FAILURE IS NOT THE VIEWPORT'S. A viewer with no
            // panel is worth far more than no viewer, so this reports and
            // carries on -- every `hud.IsReady()` below is that decision.
            // ⚠️ SAME DECISION AS THE HUD'S, for the same reason: a viewport
            // that cannot pick is worth far more than no viewport. `pickAvailable`
            // is what tells a probe which of the two happened, because "clicking
            // does nothing" looks identical either way.
            std::string pickError;
            if (!pick.Init (device, pickError))
                ArchVizLog ("Diligent viewport: picking is unavailable (" + pickError +
                            "); the viewport runs without it");
            {
                std::lock_guard<std::mutex> lock (mutex_);
                stats_.pickAvailable = pick.IsReady ();
            }

            // ⚠️ NON-FATAL, like the HUD and the shadow map. An anchor layer
            // that fails to start must not cost the user their viewer; the
            // reason goes to archviz.log and Stats().planAnchorLayerReady says
            // so, which is the difference between "anchors are off" and
            // "anchors could not be drawn".
            std::string anchorError;
            if (!planAnchors.Init (device, target.ColorFormat (), target.DepthFormat (), anchorError))
                ArchVizLog ("Diligent viewport: the plan anchor layer did not start (" + anchorError +
                            "); the viewport runs without anchors");

            std::string hudError;
            if (!hud.Init (device, target.ColorFormat (), target.DepthFormat (), hudError))
                ArchVizLog ("Diligent viewport: the ImGui HUD did not start (" + hudError +
                            "); the viewport runs without it");
            hudState.debugView = debugView_.load ();
            hudState.renderMode = renderMode_.load ();
            hudState.showCallout = showCallout_.load ();
            // ⚠️ THE OVERLAY'S HUD IS A READOUT, NOT A CONTROL SURFACE. Its
            // window is WS_EX_TRANSPARENT, so every widget would draw and none
            // could be clicked (PLAT-RE55) -- and a dead control reads as a hung
            // viewer. Set once from the surface, which never changes for a run.
            hudState.readOnly = (surface.mode == SurfaceMode::Overlay);
            // The overlay also stays on Fast: its budget belongs to Archicad, and
            // it exists to be compared AGAINST Archicad's shading.
            if (hudState.readOnly)
                hudState.renderQuality = int (RenderQuality::Fast);
        }

        uint32_t width = surface.width;
        uint32_t height = surface.height;
        uint64_t frames = 0;
        const auto started = std::chrono::steady_clock::now ();
        auto fpsStarted = std::chrono::steady_clock::now ();
        uint64_t fpsStartedFrames = 0;
        bool clearVerified = false;
        bool userHasNavigated = false;
        bool framedRealGeometry = false;
        int lastCommandedDebugView = debugView_.load ();
        // The camera numbers behind a G-buffer view, logged once per entry into
        // one. A depth view that looks wrong is otherwise unattributable: the
        // near/far/focus triple that shapes it is not on screen anywhere, and
        // "it was fine on the other machine" is the only evidence there is.
        int lastLoggedGBufferView = -1;
        int lastCommandedRenderMode = renderMode_.load ();
        // ⚠️ SEEDED FROM THE CURRENT VALUE, NOT ZERO. Starting at 0 would make
        // the first frame see a "change" and push defaults over whatever the
        // opening command already set.
        uint32_t lastEnvironmentSettingsSeq = environmentSettingsSeq_.load ();
        bool lastCommandedCallout = showCallout_.load ();
        // ⚠️ THE EDGE, NOT THE STATE. The projection is re-derived only on the
        // frame the toggle CHANGES -- see the block that reads this for why a
        // per-frame recompute would make a parallel view refuse to zoom.
        bool lastOrthographic = hudState.orthographic;
        uint64_t lastCommandedSunSeq = sunOverrideSeq_.load ();
        uint64_t lastPlanAnchorSeq = 0; // 0 so the FIRST set is always adopted
        // Everything a click and a hover remember between frames, in one place --
        // see PickState. ServicePick owns all of it; the loop only reads
        // `hoverId` back out for the callout.
        PickState pickState;

        while (!stopRequested_.load ()) {
            if (resizePending_.exchange (false)) {
                const uint32_t nextWidth = pendingWidth_.load ();
                const uint32_t nextHeight = pendingHeight_.load ();
                if (nextWidth != width || nextHeight != height) {
                    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
                    // ⚠️ QUEUED, APPLIED IN BeginFrame. The overlay's path has to
                    // drop its wrapped back-buffer views before ResizeBuffers
                    // will accept, and doing that here -- between the previous
                    // frame's bind and this frame's -- is how a resize during a
                    // drag destroys a texture the context is still holding.
                    target.RequestResize (nextWidth, nextHeight);
                    ArchVizLog ("Diligent viewport resize: " + std::to_string (width) + "x" + std::to_string (height) +
                                " -> " + std::to_string (nextWidth) + "x" + std::to_string (nextHeight));
                    width = nextWidth;
                    height = nextHeight;
                    std::lock_guard<std::mutex> lock (mutex_);
                    stats_.width = width;
                    stats_.height = height;
                    ++stats_.resizes;
                }
            }

            Diligent::ITextureView* rtv = nullptr;
            Diligent::ITextureView* dsv = nullptr;
            if (!target.BeginFrame (rtv, dsv))
                throw std::runtime_error ("the viewport surface returned no back-buffer view "
                                          "for this frame");

            gpuTimings.BeginFrame (frames);

            // Probe 1b established this sequence against the same D3D11
            // backend. Present and the one-time native readback both touch
            // state outside our ordinary draw calls, so do not trust cached
            // bindings from the previous frame.
            context->InvalidateState ();
            context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            // Deliberately unmistakable for this smoke test. The earlier dark
            // blue-green could be described as black; this cyan cannot.
            // ⚠️ THE COLOUR DEPENDS ON THE SURFACE, and it is not a preference.
            // The palette child clears to an unmistakable cyan, which is what the
            // PLAT-RE22 smoke test looks at. The overlay clears to PREMULTIPLIED
            // TRANSPARENT BLACK -- the one value that means "nothing here" on a
            // premultiplied composition chain. Clearing the overlay to cyan would
            // paint a solid rectangle over Archicad's 3D window.
            context->ClearRenderTarget (rtv, target.ClearColor (), Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            Diligent::ITextureView* none[1] = { nullptr };
            // ⚠️ THE A/B IS A PALETTE-CHILD DIAGNOSTIC AND IS SKIPPED ON THE
            // OVERLAY. It compares a readback against an OPAQUE clear colour
            // (PLAT-RE24 decoded exactly which value that is through an sRGB
            // view); against a transparent clear it would report "both clears
            // failed" every time, which is the one verdict that says the fault is
            // below Diligent. A false alarm in that slot is worse than no check.
            if (target.Mode () == SurfaceMode::Overlay)
                clearVerified = true;
            if (!clearVerified) {
                // The A/B needs the target unbound before it can copy: D3D11
                // refuses to copy a resource still bound for output, and the
                // first PLAT-RE22 diagnostic reported a zero pixel for exactly
                // that reason rather than for a rendering fault.
                context->SetRenderTargets (1, none, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
                context->Flush ();
            }
            if (!clearVerified) {
                const ClearAB ab = RunClearAB (context, rtv, target.DiligentSwapChain (), device);
                ArchVizLog ("Diligent viewport device: " + ab.device.report);
                ArchVizLog ("Diligent viewport clear A (Diligent ClearRenderTarget): " + ab.diligentArm.message);
                ArchVizLog ("Diligent viewport clear B (native ClearRenderTargetView): " + ab.nativeArm.message);
                ArchVizLog ("Diligent viewport A/B verdict: " + ab.verdict);
                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    stats_.clearChecked = true;
                    stats_.diligentClearMatched = ab.diligentArm.matched;
                    stats_.nativeClearMatched = ab.nativeArm.matched;
                    stats_.diligentClearReport = ab.diligentArm.message;
                    stats_.nativeClearReport = ab.nativeArm.message;
                    stats_.adapter = ab.device.adapter;
                    stats_.featureLevel = ab.device.featureLevel;
                    stats_.deviceRemovedReason = ab.device.deviceRemovedReason;
                    stats_.presentCount = ab.device.presentCount;
                }
                clearVerified = true;
                // The A/B leaves the magenta arm on the surface. Put the cyan
                // back so what the user is asked to look at is what the log
                // says the FIRST arm wrote.
                context->InvalidateState ();
                context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                context->ClearRenderTarget (rtv, target.ClearColor (),
                                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                context->SetRenderTargets (1, none, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
                context->Flush ();
            }

            // ---- the scene --------------------------------------------------
            // ⚠️ BOUNDED PER FRAME. A full extraction queues thousands of
            // elements; uploading them all at once stops the viewer presenting
            // for a second and takes that second out of Archicad's UI thread
            // with it. 32 is the same starting point the bgfx path uses.
            const size_t consumed = scene.Consume (device, 32);

            // ⚠️ EVERY BATCH, NOT ONLY THE FIRST. Geometry arrives 32 elements at
            // a time, so the box after batch one covers 32 elements -- and
            // `framedRealGeometry` latches on that batch. Recording bounds only
            // there would leave "zoom to fit" fitting to a corner of the building
            // for the rest of the session. Refreshing is a bounds read and a
            // couple of stores; the FRAMING below still happens exactly once.
            if (consumed > 0) {
                float boundsMin[3];
                float boundsMax[3];
                if (scene.SceneBounds (boundsMin, boundsMax))
                    camera.SetBounds (boundsMin, boundsMax);
            }

            if (consumed > 0 && !framedRealGeometry) {
                float sceneMin[3];
                float sceneMax[3];
                // ⚠️ ONLY IF ARCHICAD DID NOT GIVE US A CAMERA. Framing the
                // model would throw away the view the user was looking at, which
                // is the thing they asked to start from.
                if (!cameraStart.valid && scene.SceneBounds (sceneMin, sceneMax)) {
                    camera.FrameBounds (sceneMin, sceneMax);
                    framedRealGeometry = true;
                    ArchVizLog ("Diligent viewport framed the extracted model");
                }
                else if (cameraStart.valid) {
                    // The camera came from Archicad and must not be overridden.
                    // The box is already remembered above, so "zoom to fit" works
                    // without this path ever framing anything.
                    framedRealGeometry = true;
                }
            }

            // ⚠️ THE SHADOW PASS GOES HERE, BEFORE THE MAIN TARGETS ARE BOUND,
            // and it deliberately leaves nothing bound afterwards. It swaps the
            // render targets to its own depth texture; doing it after the
            // SetRenderTargets below would silently undo that binding and render
            // the whole frame into the shadow map.
            //
            // Skipped entirely over the floor plan: the only thing that samples
            // this map is the mesh shader, and the model is not drawn there (see
            // the note at the Draw call). Filling it anyway would be a whole
            // depth pass over the model, every frame, for a texture nothing
            // reads -- on the one path whose entire budget belongs to Archicad.
            gpuTimings.Begin (context, GpuTimingStage::VisibilityGBuffer);
            if (!camera.IsOrthographic ())
                scene.RenderShadowMap (context);
            gpuTimings.End (context, GpuTimingStage::VisibilityGBuffer);

            // ⚠️ THE DEPTH BUFFER IS BOUND HERE AND NOWHERE ELSE. The clear
            // above deliberately takes a null DSV -- clearing colour is the
            // PLAT-RE22 smoke test and must stay independent of the scene -- so
            // the render targets are set again WITH the depth view before
            // anything is drawn. Drawing with a depth-enabled PSO and no DSV
            // bound is a validation error, not a picture.
            context->SetRenderTargets (1, &rtv, dsv, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            if (dsv != nullptr)
                context->ClearDepthStencil (dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            // ---- navigation ---------------------------------------------------
            // Archicad's own conventions, through the SAME Camera the bgfx
            // viewer uses: wheel zooms toward the cursor, wheel-button drags
            // pan, Shift+drag orbits, double-click fits.
            InputSnapshot input = InputRingBuffer::Get ().Take ();
            // ⚠️ AFTER Take, BEFORE anything consumes it. Take() returns the
            // polled fields zeroed on purpose, so a consumer that runs before
            // this line sees the cursor at 0,0.
            PollHardwareInput (surface.nwh, input);
            // ⚠️ THE HUD'S ANSWER IS FROM THE PREVIOUS FRAME, AND THAT IS
            // CORRECT. ImGui only knows whether it wants the mouse after its
            // widgets have been submitted, and the HUD is drawn at the END of
            // the frame -- after the scene, because it draws over it. Waiting
            // for this frame's answer would mean drawing the HUD before the
            // scene, which puts the panel underneath the building. One frame of
            // latency on "was that click for the combo box" is invisible; the
            // alternative is an orbit every time the user opens the dropdown.
            if (camera.ApplyInput (input, hudState.wantsMouse, width, height))
                userHasNavigated = true;

            // ---- the overlay path: Archicad drives ----------------------------
            // ⚠️ AFTER ApplyInput, NOT BEFORE. A pushed camera has to be the last
            // word for the frame, or the mouse would move it right back and the
            // overlay would sit one drag out of register with the window under
            // it. In the standalone viewport nothing pushes and this never runs.
            if (cameraSyncPending_.exchange (false)) {
                CameraStart pushed;
                uint64_t pushedGeneration = 0;
                {
                    // ⚠️ THE CAMERA AND ITS GENERATION COME OUT TOGETHER, UNDER
                    // ONE LOCK. Reading the generation after unlocking let a
                    // publication land in between, stamping an OLDER camera with
                    // a NEWER generation -- which makes the frame look current
                    // and hides exactly the stale present the counter exists to
                    // find. SyncCamera writes both inside the same lock.
                    std::lock_guard<std::mutex> lock (mutex_);
                    pushed = pendingCamera_;
                    pushedGeneration = pendingCameraGeneration_;
                }
                adoptedGeneration = pushedGeneration;
                if (ApplyArchicadCamera (camera, pushed, width, height)) {
                    userHasNavigated = true; // stop the idle auto-orbit for good
                    std::lock_guard<std::mutex> lock (mutex_);
                    ++stats_.cameraSyncs;
                    stats_.cameraSource = pushed.source;
                }
            }

            // The automatic orbit exists so a STILL cube cannot be mistaken for
            // a stuck frame. It stops the moment the user takes the camera --
            // a viewport that keeps drifting under the hand is worse than no
            // motion at all.
            // ⚠️ AND NEVER WHEN ARCHICAD GAVE US A CAMERA. `SetOrbit` is a
            // perspective orbit pose -- it also leaves the top-down plan pose --
            // so on an overlay it would spin the picture for the fraction of a
            // second before the first sync lands, and over a PLAN it would
            // replace a parallel top view with a tilted one that then has to be
            // undone. The cube is what this is for, and the cube has no camera.
            if (!userHasNavigated && !framedRealGeometry && !cameraStart.valid) {
                const double age = std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count ();
                camera.SetOrbit (float (age * 0.35) - 0.9f, 0.6f);
            }

            float view[16];
            float proj[16];
            float viewProj[16];
            camera.GetViewMatrix (view);
            camera.GetProjMatrix (proj, height > 0 ? float (width) / float (height) : 1.0f);
            Multiply (viewProj, view, proj);

            float eye[3];
            camera.GetEyePosition (eye);

            // ---- PLAT-RE83: the `hideonnav` blank (see SetBlanked's header) --
            // ⚠️ READ ONCE PER FRAME, AFTER THE CLEAR AND BEFORE THE CONTENT. A
            // mid-frame change would draw the anchors and not the scene, which is
            // a picture nothing intends -- and everything above still runs, so
            // lifting the blank shows a correct overlay rather than one catching
            // up over the next few frames.
            const bool blanked = blanked_.load ();

            // ⚠️ THE MODEL IS NOT DRAWN ON THE FLOOR PLAN, AND THAT IS THE
            // DECISION PLAT-RE65 IS, not an optimisation. A floor plan is not a
            // top view of the model -- it is a separate representation Archicad
            // maintains, with its own cut plane and its own symbols. Painting a
            // shaded or wireframe 3D snapshot over it produces a confident
            // picture that is wrong in kind, which is what the first plan
            // overlay run was rejected for: "Plan view is not 3d geometry in
            // archicad its different entity. Showing stale 3d snapshot on plan
            // is wrong approach."
            //
            // What belongs here instead is the ANALYSIS LAYER and the anchors
            // below it. The wireframe's old job -- proving the camera lines up
            // -- is now the anchors' job, and they do it against Archicad's OWN
            // 2D geometry rather than against a second copy of the model.
            //
            // ⚠️ ONE CONDITION, READ BY BOTH THE ID PASS AND THE VISIBLE PASS,
            // and it is hoisted here so that stays true (PLAT-RE136). The id
            // buffer's entire premise is that it holds the picture the user is
            // looking at; a frame that picks against geometry it did not draw
            // resolves clicks to elements that are not on screen, which is the
            // hardest possible version of "picking selects the wrong thing".
            // ⚠️ THE ORTHOGRAPHIC CAMERA IS THE TEST because it is what the plan
            // path fits (PlanViewCamera -> FitPlanCamera). ⚠️ AND THAT TEST IS
            // TOO BROAD NOW -- the HUD's axonometric toggle also makes the camera
            // orthographic without making it a plan; see PLAT-RE142.
            const bool drawingOverThePlan = camera.IsOrthographic ();
            const bool modelIsDrawn = !drawingOverThePlan && !blanked;

            // ---- picking (PLAT-RE34, PLAT-RE136) -----------------------------
            // ⚠️ AFTER the camera has consumed this frame's input, and only when
            // ImGui does not want the mouse -- a click on the HUD is a click on
            // the HUD, not a selection behind it.
            //
            // ⚠️ CALLED EVEN WHEN IT CANNOT PICK, because clearing the hover is
            // one of its answers. See ServicePick.
            ServicePick (pick, pickState, !hudState.wantsMouse && modelIsDrawn, device, context, scene, input, frames,
                         width, height, viewProj, rtv, dsv, mutex_, stats_);
            const uint32_t hoverId = pickState.hoverId;

            // What the callout shows. Looked up every frame rather than cached
            // with the pick, so a live re-extraction of the hovered element
            // updates it instead of leaving stale numbers under the cursor.
            hudState.hover = hoverId != 0 ? scene.InfoForId (hoverId) : DiligentScene::ElementInfo {};

            // The SELECTED element's properties, looked up the same way and for
            // the same reason. `SelectedGuid` is the click's product; the panel
            // showing it is the whole point of picking here, since the viewer
            // never writes a selection back to Archicad.
            {
                const std::string selectedGuid = scene.PrimarySelectedGuid ();
                hudState.selected =
                    selectedGuid.empty () ? DiligentScene::ElementInfo {} : scene.InfoForGuid (selectedGuid);
            }

            // ---- the cursor's coordinate, for the callout --------------------
            // Where the view ray under the cursor meets the GROUND PLANE (z=0).
            // ⚠️ NOT the surface under the cursor -- that needs a depth readback,
            // and this path already throttles its one readback to keep a hover
            // from delaying a click. The callout labels it "on z=0" for exactly
            // that reason.
            hudState.cursorX = input.x;
            hudState.cursorY = input.y;
            hudState.cursorGroundValid = false;
            if (input.inside && height > 0 && width > 0) {
                float rayOrigin[3];
                float rayDir[3];
                camera.CursorRay (input.x, input.y, width, height, rayOrigin, rayDir);
                // A ray parallel to z=0 never meets it; one pointing away meets it
                // only behind the viewer, which is not what the cursor is over.
                if (std::abs (rayDir[2]) > 1e-6f) {
                    const float t = -rayOrigin[2] / rayDir[2];
                    if (t > 0.0f) {
                        hudState.cursorGround[0] = rayOrigin[0] + rayDir[0] * t;
                        hudState.cursorGround[1] = rayOrigin[1] + rayDir[1] * t;
                        hudState.cursorGround[2] = 0.0f;
                        hudState.cursorGroundValid = true;
                    }
                }
            }

            // ⚠️ THE HUD AND THE COMMAND ARE TWO WAYS TO SET ONE VALUE, so one
            // of them has to lose. The COMMAND wins, but only when it CHANGES:
            // `SetDiligentDebugView` writes debugView_, and the frame that sees
            // a new value there adopts it into the HUD. Any other frame, the
            // HUD's combo is authoritative. Letting the command win every frame
            // would make the combo snap back the instant it was touched; letting
            // the HUD win every frame would make the command's parameter do
            // nothing on a running viewport.
            // The banner's text and what is left of its countdown, recomputed
            // every frame -- InstructionBanner holds a deadline, not a count.
            instruction_.PublishTo (hudState);

            const int commanded = debugView_.load ();
            if (commanded != lastCommandedDebugView) {
                hudState.debugView = commanded;
                lastCommandedDebugView = commanded;
            }
            else if (hudState.debugView != commanded) {
                debugView_.store (hudState.debugView);
                lastCommandedDebugView = hudState.debugView;
            }

            // ⚠️ BEFORE Draw AND BEFORE RenderShadowMap WOULD BE BETTER STILL,
            // but the HUD only reports last frame's slider position -- it is
            // drawn at the end. Applying it here means the shadow map is one
            // frame behind the shading while the slider is being dragged, which
            // is invisible at 60 fps and costs nothing to leave.
            // A commanded sun override, adopted on the frame the generation
            // changes. See DiligentViewport::SetSunOverride for why a counter
            // rather than three last-commanded values, and why this exists at all
            // when the HUD already has the sliders.
            const uint64_t commandedSunSeq = sunOverrideSeq_.load ();
            if (commandedSunSeq != lastCommandedSunSeq) {
                lastCommandedSunSeq = commandedSunSeq;
                hudState.sunOverride = sunOverrideOn_.load ();
                hudState.sunAzimuthDegrees = sunOverrideAzimuth_.load ();
                hudState.sunAltitudeDegrees = sunOverrideAltitude_.load ();
                ArchVizLog (std::string ("Diligent viewport sun override ") + (hudState.sunOverride ? "ON" : "OFF") +
                            ": azimuth " + std::to_string (hudState.sunAzimuthDegrees) +
                            " deg (model, CCW from "
                            "+X), altitude " +
                            std::to_string (hudState.sunAltitudeDegrees) + " deg, commanded");
            }

            scene.SetSunOverride (hudState.sunOverride, hudState.sunAzimuthDegrees, hudState.sunAltitudeDegrees);

            // ---- the HDR environment --------------------------------------
            // ⚠️ THE PATH IS TAKEN UNDER THE LOCK AND THE FLAG CLEARED WITH IT.
            // Clearing the flag first would let a second request arrive between
            // the two and be swallowed -- and the symptom is "the second sky I
            // loaded did nothing", which reads as a bad file.
            if (environmentLoadPending_.load ()) {
                std::string environmentPath;
                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    environmentPath = pendingEnvironmentPath_;
                    environmentLoadPending_.store (false);
                }
                // The scene defers again, to its own Draw -- it owns the device
                // context, and this is still outside it.
                scene.SetEnvironmentMap (environmentPath.c_str ());
            }
            // ⚠️ THE COMMAND WINS ONLY WHEN IT CHANGES, then the HUD owns these
            // -- the same rule as the debug view and the render mode, and for
            // the same reason. The bus verb sets the sliders' positions; from
            // then on the sliders are authoritative, so dragging one is not
            // undone on the next frame by a value nobody touched.
            const uint32_t environmentSeq = environmentSettingsSeq_.load ();
            if (environmentSeq != lastEnvironmentSettingsSeq) {
                lastEnvironmentSettingsSeq = environmentSeq;
                hudState.environmentEnabled = environmentEnabled_.load ();
                hudState.environmentIntensity = environmentIntensity_.load ();
                hudState.environmentRotationDegrees = environmentRotationDegrees_.load ();
            }
            scene.SetEnvironmentSettings (hudState.environmentEnabled, hudState.environmentIntensity,
                                          hudState.environmentRotationDegrees);
            // ⚠️ AND'ed WITH THE SURFACE MODE, never assigned from the HUD
            // alone. The overlay must not paint an opaque sky over Archicad's
            // own 3D window, and the checkbox is drawn there (read-only) like
            // every other.
            scene.SetEnvironmentBackground (hudState.environmentBackground && surface.mode != SurfaceMode::Overlay);
            scene.SetSunWithSkyWeight (hudState.sunWithSkyWeight);
            scene.SetGrading (hudState.exposure, hudState.reflectance, hudState.roughnessBias);
            scene.SetAutoExposure (hudState.autoExposure);
            scene.SetWhiteBalance (hudState.whiteBalanceKelvin, hudState.whiteBalanceTint);

            // ⚠️ THE HUD AND THE COMMAND SHARE THE RENDER MODE, exactly as they
            // share the debug view, and with the same rule: the COMMAND wins
            // only when it CHANGES. Read the note above the debug-view block.
            const int commandedMode = renderMode_.load ();
            if (commandedMode != lastCommandedRenderMode) {
                hudState.renderMode = commandedMode;
                lastCommandedRenderMode = commandedMode;
            }
            else if (hudState.renderMode != commandedMode) {
                renderMode_.store (hudState.renderMode);
                lastCommandedRenderMode = hudState.renderMode;
            }
            const bool commandedCallout = showCallout_.load ();
            if (commandedCallout != lastCommandedCallout) {
                hudState.showCallout = commandedCallout;
                lastCommandedCallout = commandedCallout;
            }
            else if (hudState.showCallout != commandedCallout) {
                showCallout_.store (hudState.showCallout);
                lastCommandedCallout = hudState.showCallout;
            }
            scene.SetRenderMode (static_cast<SceneRenderMode> (hudState.renderMode));
            // Quality has no command yet, so the HUD is its only source and needs
            // no reconciliation -- unlike the three above, which two things set.
            scene.SetRenderQuality (hudState.renderQuality == int (RenderQuality::Realistic) ? RenderQuality::Realistic
                                                                                             : RenderQuality::Fast);

            // ---- the projection toggle --------------------------------------
            // ⚠️ SWITCHING MUST NOT RE-FRAME THE MODEL, or the two projections
            // cannot be compared -- which is the only reason to offer both. The
            // parallel half-height is derived from the CURRENT eye-to-target
            // distance and vertical FOV, so the model subtends the same angle
            // before and after: halfHeight = distance * tan(fovY/2).
            //
            // ⚠️ ONLY ON THE FRAME THE TOGGLE CHANGES. Recomputing every frame
            // would fight the user's own zoom, because in parallel projection
            // zooming CHANGES the half-height and nothing else -- so a per-frame
            // recompute would snap it back from the perspective distance and the
            // view would refuse to zoom at all.
            if (hudState.orthographic != lastOrthographic) {
                if (hudState.orthographic) {
                    constexpr float kPi = 3.14159265358979323846f;
                    const float halfHeight =
                        camera.Distance () * std::tan (camera.FovDegreesVertical () * 0.5f * (kPi / 180.0f));
                    camera.SetOrthographic (true, halfHeight > 1e-3f ? halfHeight : 1.0f);
                }
                else {
                    camera.SetOrthographic (false, 0.0f);
                }
                lastOrthographic = hudState.orthographic;
            }
            // The silhouette's thickness is in pixels and has to become NDC
            // somewhere; this is the one place that knows the surface size.
            scene.SetViewportSize (width, height);

            SetDiligentCameraRays (scene, camera, width, height);

            // Why this is conditional, and why the id pass reads the same flag,
            // is at `modelIsDrawn` above -- next to the picking that depends on it.
            if (modelIsDrawn) {
                gpuTimings.Begin (context, GpuTimingStage::Shading);
                const bool gBufferDebugView = hudState.debugView == int (DiligentDebugView::GBufferNormals) ||
                                              hudState.debugView == int (DiligentDebugView::GBufferDepth) ||
                                              hudState.debugView == int (DiligentDebugView::AmbientOcclusion) ||
                                              hudState.debugView == int (DiligentDebugView::GBufferAlbedo) ||
                                              hudState.debugView == int (DiligentDebugView::GBufferRoughness) ||
                                              hudState.debugView == int (DiligentDebugView::GBufferMaterialData);
                if (gBufferDebugView) {
                    if (hudState.debugView != lastLoggedGBufferView) {
                        lastLoggedGBufferView = hudState.debugView;
                        ArchVizLog ("Diligent G-buffer view " + std::to_string (hudState.debugView) + ": near " +
                                    std::to_string (Camera::NearClip ()) + " m, far " +
                                    std::to_string (camera.FarClip ()) + " m, eye-to-target " +
                                    std::to_string (camera.Distance ()) + " m, " +
                                    (camera.IsPerspective () ? "perspective" : "parallel"));
                    }
                    scene.DrawGBufferDebug (context, rtv, view, proj, viewProj, eye, Camera::NearClip (),
                                            camera.FarClip (), camera.Distance (), camera.IsPerspective (),
                                            static_cast<uint32_t> (frames), CullMode::Cw, hudState.debugView);
                }
                else {
                    // Re-arm the log, so switching away and back reports the
                    // camera as it is THEN rather than staying silent.
                    lastLoggedGBufferView = -1;
                    scene.Draw (context, viewProj, eye, CullMode::Cw, hudState.debugView);
                }
                gpuTimings.End (context, GpuTimingStage::Shading);
            }
            else {
                gpuTimings.Begin (context, GpuTimingStage::Shading);
                gpuTimings.End (context, GpuTimingStage::Shading);
            }

            // ---- PLAT-RE65: Archicad's own 2D outlines, over everything -----
            gpuTimings.Begin (context, GpuTimingStage::Post);
            UpdateAndDrawPlanAnchors (planAnchors, device, context, mutex_, pendingPlanAnchors_, planAnchorSeq_.load (),
                                      lastPlanAnchorSeq, planAnchorsOn_.load () && !blanked, viewProj, width, height,
                                      planAnchorWidthPixels_.load (), planAnchorRgba_.load ());

            if (!blanked)
                DrawCornerGnomon (context, scene, rtv, dsv, camera, width, height);

            // Last, over everything, into the full-surface viewport the gnomon
            // restored.
            if (hud.IsReady () && !blanked) {
                const DiligentSceneStats hudScene = scene.Stats ();
                {
                    // ⚠️ THE LOCK ENDS HERE, BEFORE Draw. `mutex_` is what the
                    // palette's state command takes; holding it across a GPU
                    // submission would block Archicad's main thread for as long
                    // as the driver takes.
                    std::lock_guard<std::mutex> lock (mutex_);
                    hudState.fps = stats_.fps;
                    hudState.adapter = stats_.adapter;
                }
                hudState.frames = frames;
                hudState.width = width;
                hudState.height = height;
                hud.Draw (context, width, height, input, hudScene, hudState);
            }
            gpuTimings.End (context, GpuTimingStage::Post);
            // The current renderer is a single-sample forward raster path. Keep
            // the field explicit so a progressive accumulator can replace this
            // value without changing the benchmark log contract.
            gpuTimings.EndFrame (frames, 1);

            context->SetRenderTargets (1, none, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);

            ApplyRequestedFrameLatency (target, requestedFrameLatency_.load (), appliedLatency);
            // To Archicad's Present detour, before ours: this frame's back
            // buffer is still buffer 0 (PLAT-RE79 phase 4).
            MirrorOverlayToHost (device, context, target, camera);
            PresentAndAccount (target, publishedCameraGeneration_, adoptedGeneration, stalePresents_,
                               presentedCameraGeneration_, mutex_, stats_);
            ++frames;

            // Desync measurement (PLAT-RE84). ⚠️ AFTER Present, so the
            // timestamp is when the frame reached the swap chain.
            LogPresentedPlanFrame (camera, width, height, frames);

            const auto now = std::chrono::steady_clock::now ();
            const double elapsed = std::chrono::duration<double> (now - fpsStarted).count ();
            if (elapsed >= 0.5) {
                // Refresh the DXGI present count alongside the frame count: the
                // A/B reads it before the first Present, so its value there is
                // always 0, and a loop that spins without presenting is exactly
                // what these two numbers diverging would show.
                UINT presents = 0;
                if (dxgiSwapChain != nullptr)
                    dxgiSwapChain->GetLastPresentCount (&presents);

                const DiligentSceneStats sceneStats = scene.Stats ();

                std::lock_guard<std::mutex> lock (mutex_);
                stats_.frames = frames;
                stats_.fps = double (frames - fpsStartedFrames) / elapsed;
                stats_.presentCount = presents;
                stats_.sceneElements = sceneStats.elements;
                stats_.sceneTriangles = sceneStats.triangles;
                stats_.sceneVertices = sceneStats.vertices;
                stats_.sceneGpuBytes = sceneStats.gpuBytes;
                stats_.scenePending = sceneStats.pending;
                stats_.sceneMaterials = sceneStats.materials;
                stats_.materialMisses = sceneStats.materialMisses;
                stats_.transparentRanges = sceneStats.transparentRanges;
                stats_.sunApplied = sceneStats.sunApplied;
                stats_.sunBelowHorizon = sceneStats.sunBelowHorizon;
                stats_.sun[0] = sceneStats.sun[0];
                stats_.sun[1] = sceneStats.sun[1];
                stats_.sun[2] = sceneStats.sun[2];
                stats_.ambient = sceneStats.ambient;
                stats_.sunOverridden = sceneStats.sunOverridden;
                stats_.sunAzimuthDegrees = sceneStats.sunAzimuthDegrees;
                stats_.sunBearingDegrees = sceneStats.sunBearingDegrees;
                stats_.northDegrees = sceneStats.northDegrees;
                stats_.sunAltitudeDegrees = sceneStats.sunAltitudeDegrees;
                stats_.latitudeDegrees = sceneStats.latitudeDegrees;
                stats_.longitudeDegrees = sceneStats.longitudeDegrees;
                stats_.siteAltitudeMetres = sceneStats.siteAltitudeMetres;
                stats_.year = sceneStats.year;
                stats_.month = sceneStats.month;
                stats_.day = sceneStats.day;
                stats_.hour = sceneStats.hour;
                stats_.minute = sceneStats.minute;
                stats_.summerTime = sceneStats.summerTime;
                stats_.haveComputedSun = sceneStats.haveComputedSun;
                stats_.computedAzimuthDegrees = sceneStats.computedAzimuthDegrees;
                stats_.computedAltitudeDegrees = sceneStats.computedAltitudeDegrees;
                stats_.shadowReady = sceneStats.shadowReady;
                stats_.shadowFitted = sceneStats.shadowFitted;
                stats_.shadowResolution = sceneStats.shadowResolution;
                stats_.shadowTexelMetres = sceneStats.shadowTexelMetres;
                stats_.environmentLoaded = sceneStats.environmentLoaded;
                stats_.environmentActive = sceneStats.environmentActive;
                stats_.environmentMipLevels = sceneStats.environmentMipLevels;
                stats_.environmentAverage[0] = sceneStats.environmentAverage[0];
                stats_.environmentAverage[1] = sceneStats.environmentAverage[1];
                stats_.environmentAverage[2] = sceneStats.environmentAverage[2];
                stats_.environmentPath = sceneStats.environmentPath;
                stats_.environmentError = sceneStats.environmentError;
                stats_.selectedCount = sceneStats.selected;
                stats_.planAnchors = planAnchorsOn_.load ();
                stats_.planAnchorLayerReady = planAnchors.IsReady ();
                stats_.planAnchorVertices = uint64_t (planAnchors.VertexCount ());
                stats_.planAnchorWidthPixels = planAnchorWidthPixels_.load ();
                // The live camera, for the overlay sync test to compare against
                // Archicad's own numbers.
                float target[3];
                camera.GetTarget (target);
                for (int k = 0; k < 3; ++k) {
                    stats_.cameraEye[k] = eye[k];
                    stats_.cameraTarget[k] = target[k];
                }
                stats_.cameraFovDegreesVertical = camera.FovDegreesVertical ();
                fpsStarted = now;
                fpsStartedFrames = frames;
            }
        }

        // Read the device once more on the way out: a device lost mid-run keeps
        // the frame counter climbing, and only deviceRemovedReason says so.
        const DeviceDiagnostics closing = DescribeDevice (device, target.DiligentSwapChain ());
        {
            std::lock_guard<std::mutex> lock (mutex_);
            stats_.presentCount = closing.presentCount;
            stats_.deviceRemovedReason = closing.deviceRemovedReason;
        }
        ArchVizLog ("Diligent viewport closing: " + closing.report);

        // ---- teardown -------------------------------------------------------
        // ⚠️ THIS IS WHAT LETS THE VIEWPORT BE OPENED A SECOND TIME. Without it,
        // the first close leaves the HWND still associated with a live DXGI swap
        // chain, every later CreateSwapChainD3D11 on that window fails, and the
        // only cure is restarting Archicad -- exactly what was reported: a
        // viewport that closed cleanly, then six "returned no swap chain"
        // failures in a row while the palette window sat there looking fine.
        //
        // D3D11's rule for FLIP-model swap chains (DXGI reported effect=3,
        // FLIP_DISCARD, so it is this one) is that releasing the swap chain is
        // not enough: the immediate context still holds references to its back
        // buffers, destruction is deferred, and the HWND stays taken. The
        // documented fix is ClearState then Flush on the NATIVE context --
        // Diligent's InvalidateState only drops its own state cache, and
        // SetRenderTargets(0,...) unbinds render targets but not the shader
        // resources, samplers and vertex buffers that also hold references.
        releaseEverything ();
        ArchVizLog ("Diligent viewport: stopped and released after " + std::to_string (frames) + " frames");
    }
    catch (const std::exception& ex) {
        // ⚠️ THE SAME TEARDOWN ON THE FAILURE PATH. A shader that will not
        // compile leaves a perfectly good swap chain behind, and unwinding it
        // without the ClearState above costs the user the rest of their Archicad
        // session on that window -- turning one bad build into "the viewport is
        // permanently broken".
        releaseEverything ();
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.failed = true;
        stats_.error = ex.what ();
        ArchVizLog ("Diligent viewport FAILED: " + stats_.error);
    }
    catch (...) {
        releaseEverything ();
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.failed = true;
        stats_.error = "unknown exception during Diligent viewport lifecycle";
        ArchVizLog ("Diligent viewport FAILED: " + stats_.error);
    }

    running_.store (false);
    std::lock_guard<std::mutex> lock (mutex_);
    stats_.running = false;
}

} // namespace geomsrv::archviz
