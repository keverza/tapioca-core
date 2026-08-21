#ifndef EVP_ARCHVIZ_DILIGENTVIEWPORT_HPP
#define EVP_ARCHVIZ_DILIGENTVIEWPORT_HPP

#include "ArchViz/InstructionBanner.hpp"
#include "ArchViz/PlanAnchorRibbon.hpp"
#include "ArchViz/ViewerHost.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace geomsrv::archviz {

struct DiligentViewportStats {
    bool running = false;
    bool initialized = false;
    // Which surface it came up on. ⚠️ REPORTED RATHER THAN INFERRED: an overlay
    // and a palette child are both "the Diligent viewport running", and a probe
    // asking "why is the picture not over the 3D window" has no other way to
    // tell that it opened in the palette instead.
    bool overlay = false;
    bool failed = false;
    std::string error;
    uint64_t frames = 0;
    double fps = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t resizes = 0;

    // PLAT-RE22 diagnostics, filled once on the first frame. The A/B exists
    // because a black viewport has two very different causes: Diligent's
    // state/dispatch, or the D3D11 presentation path underneath it. Clearing
    // the SAME view twice, once through each, separates them in one frame.
    bool clearChecked = false;
    bool diligentClearMatched = false;
    bool nativeClearMatched = false;
    std::string diligentClearReport;
    std::string nativeClearReport;
    std::string adapter;            // GraphicsAdapterInfo::Description
    uint32_t featureLevel = 0;      // D3D_FEATURE_LEVEL as a raw number
    uint64_t presentCount = 0;      // IDXGISwapChain::GetLastPresentCount
    // ---- PLAT-RE99: what the desync measurement cannot see -----------------
    uint64_t stalePresents = 0;     // frames presented carrying a superseded camera
    uint64_t presentFailures = 0;   // Present returned a failure HRESULT
    uint32_t lastPresentResult = 0; // the last non-S_OK one, e.g. DXGI_STATUS_OCCLUDED
    uint32_t frameLatency = 0;      // 0 = DXGI's default of 3
    uint32_t deviceRemovedReason = 0;   // 0 == S_OK == the device is alive

    // The scene, once the shaders and pipeline states are up. ⚠️ `sceneReady`
    // false with `initialized` true is the interesting state: the device and
    // swap chain came up and the SHADERS did not, which from the palette looks
    // like an empty viewport and nothing else.
    bool sceneReady = false;
    uint64_t sceneElements = 0;
    uint64_t sceneTriangles = 0;
    uint64_t sceneVertices = 0;
    uint64_t sceneGpuBytes = 0;
    uint64_t scenePending = 0;
    uint64_t sceneMaterials = 0;
    uint64_t materialMisses = 0;
    uint64_t transparentRanges = 0;
    // ⚠️ `sunApplied` false means no SetEnvironment ever arrived and the shader
    // is on a hardcoded default -- by eye that is indistinguishable from a real
    // sun, and it is the first thing to check when the model reads flat.
    bool sunApplied = false;
    bool sunBelowHorizon = false;
    float sun[3] = {0.0f, 0.0f, 1.0f};
    float ambient = 0.35f;
    // The sun ACTUALLY IN USE, and whether the HUD's slider replaced
    // Archicad's. ⚠️ Reported separately so a log can never claim
    // Archicad's sun while an overridden one is on screen.
    bool sunOverridden = false;
    // Two different quantities -- see DiligentSceneStats for which is which.
    float sunAzimuthDegrees = 0.0f;   // model space, CCW from +X
    float sunBearingDegrees = 0.0f;   // compass, CW from north
    float northDegrees = 90.0f;
    float sunAltitudeDegrees = 0.0f;
    // ⚠️ WHERE THAT SUN CAME FROM. `sunAzimuth/AltitudeDegrees` are Archicad's
    // STORED angles -- the ones its own 3D window shades with -- and
    // `computed*` is what this place and date would imply instead. They differ
    // whenever the sun was typed into the Sun dialog rather than derived from a
    // date, which is the ordinary case and is NOT a fault in either number.
    float latitudeDegrees = 0.0f;
    float longitudeDegrees = 0.0f;
    float siteAltitudeMetres = 0.0f;
    uint16_t year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool summerTime = false;
    bool haveComputedSun = false;
    float computedAzimuthDegrees = 0.0f;
    float computedAltitudeDegrees = 0.0f;
    // ⚠️ THESE THREE SEPARATE THE WAYS A SCENE CAN COME BACK WITHOUT SHADOWS,
    // which by eye are one symptom. `shadowResolution` 0: the depth texture or
    // its pipeline never came up, and the reason is in archviz.log.
    // `shadowReady` true with `shadowFitted` false: the map exists but no
    // frustum could be fitted, so there is no geometry yet or the sun vector is
    // degenerate. Both true and still no shadows on screen is a real bug, and
    // `debug_view=shadow` is the next look.
    bool shadowReady = false;
    bool shadowFitted = false;
    uint64_t shadowResolution = 0;
    float shadowTexelMetres = 0.0f;

    // ---- the HDR environment (PLAT-RE51) ------------------------------------
    // ⚠️ `environmentAverage` IS THE ONLY THING SEPARATING A BLACK SKY FROM AN
    // UNBOUND ONE -- they render identically. `environmentError` is the only
    // place a DEFERRED load's failure survives, because SetEnvironmentMap
    // returns long before the load is attempted.
    bool environmentLoaded = false;
    bool environmentActive = false;
    uint64_t environmentMipLevels = 0;
    float environmentAverage[3] = {0.0f, 0.0f, 0.0f};
    std::string environmentPath;
    std::string environmentError;
    // ---- RE51.B6: is the mip chain a real GGX prefilter, or the box fallback?
    // ⚠️ NOTHING ON SCREEN SEPARATES THEM. Both are "blurrier at higher
    // roughness"; the difference is whether a mirror reflects a recognisable
    // environment, which is a judgement rather than an observation.
    bool environmentPrefiltered = false;
    uint64_t environmentPrefilteredMips = 0;
    double environmentPrefilterMs = 0.0;
    std::string environmentPrefilterError;

    // ---- RE51.B9: the exposure the light implies -----------------------------
    // ⚠️ `autoExposure` IS REPORTED EVEN WHEN `autoExposureEnabled` IS FALSE.
    // That is the point: the estimate ships switched off until one live run says
    // whether its middle-grey target suits this project, and it cannot say that
    // unless the number is visible while the fixed value is what renders.
    bool autoExposureEnabled = false;
    float autoExposure = 0.0f;
    float appliedExposure = 0.0f;
    float sceneLuminance = 0.0f;
    float meanAlbedo = 0.0f;
    float whiteBalanceGains[3] = {1.0f, 1.0f, 1.0f};

    // ---- RE51.B2: the substance join's coverage ------------------------------
    uint64_t substanceNamed = 0;
    uint64_t substanceCounts[7] = {};

    std::string cameraSource;   // where the starting view came from
    // ---- the live camera, for the overlay path -----------------------------
    // What the viewport is ACTUALLY looking through, this frame. The overlay
    // sync test compares these against Archicad's own numbers; without them
    // "the two views agree" can only be judged by eye, which is exactly the
    // judgement that cannot detect a slow drift or a fixed offset.
    float cameraEye[3] = {0.0f, 0.0f, 0.0f};
    float cameraTarget[3] = {0.0f, 0.0f, 0.0f};
    float cameraFovDegreesVertical = 0.0f;
    uint64_t cameraSyncs = 0;   // how many pushed cameras have been adopted

    // ---- picking and selection (PLAT-RE34) ---------------------------------
    // ⚠️ `pickSeq` IS THE SIGNAL, NOT `pickedGuid`. Clicking the same element
    // twice leaves the guid unchanged, and a bridge watching the string would
    // ignore the second click -- including the very common "click the same thing
    // again after deselecting it in Archicad". The counter says "a pick
    // happened"; the guid says what it found, and EMPTY MEANS THE SKY, which is
    // a real answer (deselect) rather than a missing one.
    uint64_t pickSeq = 0;
    std::string pickedGuid;
    // ⚠️ THE GUID THE MODELER GAVE US IS OFTEN A SUB-PART. A curtain-wall panel,
    // a stair tread, a column segment: the 3D model enumerates those, not their
    // owner, and `ACAPI_Selection_SetSelectedElementNeig` refuses them. This is
    // reported alongside so a probe can say WHICH of the two a click produced --
    // the difference between "picking is broken for stairs" and "picking works
    // and the bridge dropped it" is otherwise invisible.
    bool pickAvailable = false;   // false = the pick target did not come up
    uint64_t selectedCount = 0;   // how many elements draw highlighted
    // PLAT-RE65's plan anchors -- Archicad's OWN 2D outlines drawn over the
    // plan so the overlay's register can be checked. ⚠️ `planAnchorVertices` 0
    // with `planAnchors` true is the diagnosis for "I turned them on and see
    // nothing": the layer is live and was handed no geometry, which is a
    // different fault from the layer having failed to start.
    bool planAnchors = false;
    bool planAnchorLayerReady = false;
    uint64_t planAnchorVertices = 0;
    float planAnchorWidthPixels = 0.0f;
};

// Where the camera starts, read from ARCHICAD'S OWN 3D WINDOW by the palette
// before the thread is launched.
//
// ⚠️ IT IS READ ON THE MAIN THREAD AND PASSED BY VALUE. `ACAPI_View_Get3DProjectionSets`
// is ACAPI, so the render thread may never call it; the palette's posted
// handler already runs on the main thread, which is the one place this can be
// asked for without a gate hop.
//
// ⚠️ Archicad's `viewCone` is a HORIZONTAL field of view in DEGREES and the
// camera's is VERTICAL. The conversion needs the aspect ratio, so it happens in
// the render thread once the viewport size is known -- not here.
struct CameraStart {
    bool valid = false;         // false: no perspective camera to copy; frame the scene instead
    float target[3] = {0.0f, 0.0f, 0.0f};
    float eye[3] = {0.0f, 0.0f, 0.0f};
    float viewConeDegreesHorizontal = 0.0f;
    std::string source;         // "perspective", "axonometric", or why it failed

    // ---- a 2D drawing window (the plan overlay) ----------------------------
    // ⚠️ WHEN THIS IS SET, `eye` AND `viewCone` ARE MEANINGLESS and must not be
    // read: a floor plan is a parallel projection straight down, so there is no
    // eye position and no cone. `target` is the model point under the CENTRE of
    // the window; the rest of the pose is these three numbers. See
    // ArchViz/PlanCameraMath.hpp for where they come from and why they are
    // measured rather than derived from the zoom box.
    bool orthographic = false;
    float orthoHalfHeightMetres = 0.0f;   // half the window's height, in model metres
    float planRotationRadians = 0.0f;     // CCW angle of the screen's +X in model space

    // ⚠️ THE VIEW WAS PROVABLY MOVING WHILE THIS WAS READ (PLAT-RE82). Set when
    // the plan reader's tear check catches Archicad scrolling BETWEEN its corner
    // samples -- direct evidence of navigation in progress, available a full tick
    // earlier than "the camera differs from last time", which cannot be known
    // until the tick AFTER the motion started. `hideonnav` uses it to blank on
    // the first moving frame instead of the second.
    bool viewMoving = false;
};

// The viewport's render thread. It owns every Diligent object it creates and
// never calls DG, ACAPI, or MainThreadGate. The palette supplies a validated
// HWND plus the starting camera, and communicates only through atomics, the
// lock-free queues (SceneCmdQueue, InputRingBuffer) and copied stats.
class DiligentViewport final {
public:
    static DiligentViewport& Get ();

    bool Start (const Surface& surface, const CameraStart& camera = CameraStart {});
    void Stop ();
    void RequestResize (uint32_t width, uint32_t height);
    void SetDebugView (int view) { debugView_.store (view); }
    int  DebugView () const { return debugView_.load (); }
    // SceneRenderMode as an int: shaded, wireframe, or both. ⚠️ WIREFRAME IS THE
    // OVERLAY'S REQUIREMENT -- a shaded viewer over Archicad's own 3D window
    // simply HIDES it, so "do the two agree" stops being answerable at the moment
    // it is being asked.
    void SetRenderMode (int mode) { renderMode_.store (mode); }
    int  RenderMode () const { return renderMode_.load (); }
    // ---- the sun override, from a COMMAND as well as the HUD ----------------
    // ⚠️ IT IS A MEASURING INSTRUMENT, NOT A LIGHTING CONTROL (HudState says
    // why), AND THE HUD CANNOT BE CLICKED ON THE OVERLAY. The overlay surface is
    // WS_EX_TRANSPARENT, so every widget on it draws and none of them can be
    // touched (PLAT-RE55) -- which left the one tool for deciding whether the
    // shadows are cast from a MIRRORED sun unusable in the only mode where the
    // question can be put side by side with Archicad's own shading.
    //
    // Setting it publishes a new generation; the render thread adopts the whole
    // triple on the frame that sees one, and the HUD's own sliders own it on
    // every other frame -- the same "the command wins only when it CHANGES" rule
    // the debug view and the render mode follow, with one counter standing in
    // for three last-commanded values.
    void SetSunOverride (bool on, float azimuthDegrees, float altitudeDegrees);

    // ---- the HDR environment (PLAT-RE51) ------------------------------------
    //
    // ⚠️ A PATH IS NOT A CAMERA OR A SUN, so this is a MUTEX-GUARDED STRING
    // rather than the atomic-plus-generation pattern above it. Those carry a few
    // floats the render thread re-reads every frame; this carries a filename the
    // render thread must read EXACTLY ONCE and then act on for hundreds of
    // milliseconds. A generation counter over a std::string would be a data race
    // the counter merely hides.
    //
    // The load itself happens on the render thread -- SetEnvironmentMap only
    // parks the request -- so a failure surfaces in the scene stats a frame or
    // two later, never as a return value here.
    void SetEnvironmentMap (const std::string& path);
    void SetEnvironmentSettings (bool enabled, float intensity, float rotationDegrees);

    // ---- PLAT-RE65: Archicad's own 2D plan outlines, drawn as anchors ------
    // `outlines` is one entry per ring, each {x,y,x,y,...} in world metres with
    // no closing repeat, paired with one arc angle per point (empty = straight).
    //
    // ⚠️ THE WHOLE SET, REPLACING, and the SEQUENCE is the signal rather than
    // the contents: re-sending the same walls must still redraw them, because
    // that is what "refresh the anchors after I moved a wall" means and the
    // guids would be identical.
    //
    // ⚠️ THE RIBBON IS BUILT HERE, ON THE CALLING THREAD, NOT ON THE RENDER
    // THREAD. It is pure arithmetic over data the caller already holds, and
    // doing it in the frame loop would put a whole storey's tessellation
    // between two presents.
    void SetPlanAnchors (const std::vector<std::vector<float>>& outlines,
                         const std::vector<std::vector<float>>& arcs,
                         bool enabled, float widthPixels, uint32_t rgba,
                         float arcSign, float planZ);

    // The mouse-following element callout.
    // ⚠️ BLANK IS NOT HIDE, AND THE DIFFERENCE IS THE POINT (PLAT-RE83). The
    // frame still runs and still Presents -- it just draws NOTHING, so the
    // composition surface is uniformly transparent and Archicad shows through
    // untouched. ShowWindow would be the obvious alternative and is worse here:
    // it disturbs z-order (which PLAT-RE64 spent two sessions settling), it can
    // steal or drop activation, and a hidden window's swap chain is a resize
    // hazard. A transparent frame costs one clear.
    //
    // This exists for the `hideonnav` experiment: hide the overlay WHILE the
    // user navigates and bring it back, correctly registered, when they stop.
    // If following can never keep up during motion, not drawing during motion is
    // the honest alternative -- and whether that FEELS better is a question only
    // a live look can answer.
    void SetBlanked (bool on) { blanked_.store (on); }
    bool Blanked () const { return blanked_.load (); }

    void SetCallout (bool on) { showCallout_.store (on); }
    bool Callout () const { return showCallout_.load (); }

    // The instruction banner across the top of the overlay (PLAT-RE111). Empty
    // text hides it; a negative `seconds` shows the text with no countdown.
    //
    // ⚠️ THIS IS THE ONLY WAY TO TELL THE USER ANYTHING WHILE THEY NAVIGATE.
    // Archicad's DG palette does not repaint during a navigation drag, so a
    // status line written from Python is frozen for the whole gesture -- which
    // is precisely the interval a measurement run needs to talk during. The
    // overlay renders every frame regardless.
    //
    // ⚠️ A DEADLINE IS STORED, NOT A COUNT. The render thread recomputes what
    // is left every frame, so the countdown ticks in real time instead of in
    // whatever steps a Python caller manages to send over the bus -- and a
    // caller that stops sending leaves a banner that finishes counting down and
    // clears itself rather than one frozen at "4".
    void SetInstruction (const std::string& text, double seconds)
    { instruction_.Set (text, seconds); }
    bool IsRunning () const { return running_.load (); }
    DiligentViewportStats Stats () const;

    // ---- the overlay path: Archicad drives the camera ----------------------
    // Push a new camera in while the viewport runs. The render thread adopts it
    // at the top of the next frame.
    //
    // ⚠️ IT OVERRIDES THE USER'S OWN NAVIGATION, and that is what an overlay
    // means: the picture has to agree with the 3D window underneath it, so
    // whoever is driving that window is driving this one. In the standalone
    // viewport nothing calls this and the mouse keeps the camera.
    //
    // ⚠️ CALLABLE FROM ANY THREAD, but the CameraStart must have been READ on
    // the main thread -- ACAPI_View_Get3DProjectionSets is ACAPI. This takes a
    // plain value precisely so the reading and the pushing can be on different
    // threads.
    //
    // ⚠️ IT IS REFUSED ON THE PANEL, AND THAT REFUSAL IS THE PANEL'S CONTRACT
    // (PLAT-RE124). The panel viewport and the overlay are the SAME singleton, so
    // arming a camera-sync mode for the overlay used to drag the panel's camera
    // too -- the panel would silently stop being a standalone viewer the moment
    // anything else armed a poll. The panel syncs ONCE, from the CameraStart
    // handed to Start(), and navigates on its own thereafter.
    //
    // The guard lives HERE rather than in the poll because there is more than one
    // way to reach a continuous push, and a rule enforced at each call site is a
    // rule that a future third call site will not know about -- the same lesson
    // the overlay's second arm path taught (ArchVizPanel::OpenDiligentOverlay).
    // An EXPLICIT one-shot is a different intent and goes through AdoptCamera.
    void SyncCamera (const CameraStart& camera);

    // Apply a camera REGARDLESS of surface mode: an explicit, one-shot request
    // rather than a continuous follow. This is what `Tapioca.SetDiligentCamera`
    // and the HUD's "Sync camera from Archicad" button use, and it is the only
    // way Archicad's camera reaches the panel after it opens.
    void AdoptCamera (const CameraStart& camera);

    // WHERE this run's frames are going. Meaningful only while IsRunning().
    SurfaceMode Mode () const { return mode_.load (); }

    // How many finished frames DXGI may queue before showing one. 0 leaves
    // DXGI's default of 3 -- see DiligentViewportTarget.hpp for why this is the
    // one lever prediction cannot substitute for. Main thread; the render thread
    // applies it.
    void SetFrameLatency (uint32_t frames) { requestedFrameLatency_.store (frames); }
    uint32_t RequestedFrameLatency () const { return requestedFrameLatency_.load (); }

private:
    DiligentViewport () = default;
    ~DiligentViewport ();
    DiligentViewport (const DiligentViewport&) = delete;
    DiligentViewport& operator= (const DiligentViewport&) = delete;

    void Run (Surface surface, CameraStart camera);

    std::thread worker_;
    std::atomic<bool> running_ {false};
    // Set by Start before the worker exists and read by SyncCamera from the main
    // thread, so it cannot be a plain member of the render thread's frame loop.
    std::atomic<SurfaceMode> mode_ {SurfaceMode::PaletteChild};
    std::atomic<bool> stopRequested_ {false};
    std::atomic<bool> resizePending_ {false};
    std::atomic<uint32_t> pendingWidth_ {0};
    std::atomic<uint32_t> pendingHeight_ {0};
    // The shader's debug view (DiligentShaders.hpp -> DiligentDebugView), set
    // from a command while the viewport runs. Atomic because it is written from
    // the bus thread and read by the render thread every frame.
    std::atomic<int> debugView_ {0};
    // Same shape and the same two-writer rule as debugView_: a bus command and
    // the HUD both set these, and the reconciliation is in the frame loop.
    std::atomic<int> renderMode_ {0};
    std::atomic<bool> showCallout_ {false};
    std::atomic<bool> blanked_ {false};
    // The banner, with its own mutex and its own expiry -- see
    // InstructionBanner.hpp. It carries a std::string, so it cannot be an
    // atomic, and it is deliberately NOT under `mutex_`: nothing about a line of
    // text needs to be consistent with the pending camera.
    InstructionBanner instruction_;
    // The commanded sun override. ⚠️ THE SEQUENCE IS THE SIGNAL, not the three
    // values: setting the same azimuth twice must still be adopted, because the
    // HUD may have moved it in between and "put it back where I asked" is the
    // whole point of an A/B.
    std::atomic<bool> sunOverrideOn_ {false};
    // ---- the HDR environment ------------------------------------------------
    // The path is under `mutex_` (see SetEnvironmentMap); the settings are
    // atomics with a sequence counter, following the sun override's rule that a
    // command wins only on the frame it CHANGES so the HUD owns it otherwise.
    std::string pendingEnvironmentPath_;
    std::atomic<bool> environmentLoadPending_ {false};
    std::atomic<bool> environmentEnabled_ {true};
    std::atomic<float> environmentIntensity_ {1.0f};
    std::atomic<float> environmentRotationDegrees_ {0.0f};
    std::atomic<uint32_t> environmentSettingsSeq_ {0};

    std::atomic<float> sunOverrideAzimuth_ {135.0f};
    std::atomic<float> sunOverrideAltitude_ {45.0f};
    std::atomic<uint64_t> sunOverrideSeq_ {0};
    // A camera pushed in from outside, waiting for the render thread. Guarded by
    // `mutex_` rather than being an atomic, because a CameraStart is seven floats
    // and a string: a torn read here would point the camera at a position from
    // one frame and a target from another, which is a jitter nobody would
    // attribute to a data race.
    std::atomic<bool> cameraSyncPending_ {false};
    // 1 by default: the 2026-08-14 review's cheapest candidate fix for the
    // lingering afterimage. Set 0 to restore DXGI's default and A/B it.
    std::atomic<uint32_t> requestedFrameLatency_ {1};

    // ---- camera generation (PLAT-RE99) -------------------------------------
    // ⚠️ IT ANSWERS A QUESTION THE DESYNC MEASUREMENT STRUCTURALLY CANNOT. That
    // measurement compares the camera a frame CARRIED against Archicad's at the
    // same instant -- it can report near-zero while a visibly stale frame is
    // still on screen, because it timestamps submission, not display, and
    // because a frame already in flight cannot be recalled when a newer camera
    // arrives. `publishedCameraGeneration_` counts what the main thread has
    // pushed; the render thread records which one it adopted, and comparing the
    // two immediately before Present says how often we present a camera we
    // already knew was out of date.
    std::atomic<uint64_t> publishedCameraGeneration_ {0};
    // The generation of whatever is sitting in `pendingCamera_`, written
    // under the same lock so the pair can never be torn apart.
    uint64_t pendingCameraGeneration_ = 0;
    std::atomic<uint64_t> presentedCameraGeneration_ {0};
    std::atomic<uint64_t> stalePresents_ {0};
    // PLAT-RE65's plan anchors. Same generation rule as the sun override: the
    // SEQUENCE is the signal, so re-sending the same walls still redraws them —
    // which is what a "refresh the anchors" command has to mean after the user
    // has edited a wall.
    std::atomic<bool> planAnchorsOn_ {false};
    std::atomic<float> planAnchorWidthPixels_ {2.0f};
    std::atomic<uint32_t> planAnchorRgba_ {0xFF3B30C0u};
    std::atomic<uint64_t> planAnchorSeq_ {0};

    mutable std::mutex mutex_;
    CameraStart pendingCamera_;
    // Guarded by `mutex_`, not atomic: this is a whole storey's triangles and a
    // torn read would draw half of one anchor set over half of another.
    std::vector<PlanAnchorVertex> pendingPlanAnchors_;
    DiligentViewportStats stats_;
};

} // namespace geomsrv::archviz

#endif
