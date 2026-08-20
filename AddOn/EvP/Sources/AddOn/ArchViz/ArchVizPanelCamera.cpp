// ArchViz/ArchVizPanelCamera -- the palette's ACAPI camera reads, and the two
// Win32 timers that drive them.
//
// WHY IT IS NOT IN ArchVizPanel.cpp. That file owns the DG palette: its items,
// its layout, its events and the renderer's lifecycle. This one owns a different
// thing that happens to hang off the same class -- ASKING ARCHICAD WHERE IT IS
// LOOKING, once at open and then sixty times a second for as long as an overlay
// is up. They were one file until the plan-view camera arrived and pushed it past
// the size cap (CLAUDE.md, architecture_check.py), and the seam was already
// there: nothing below touches a DG item.
//
// ⚠️ EVERYTHING HERE IS MAIN-THREAD ONLY. Every function calls ACAPI, and the
// two timers are `SetTimer (nullptr, ...)` precisely so their WM_TIMER is
// dispatched on the main thread -- see StartCameraSync's note for why a
// MainThreadCommand could not be used instead.

#include "ArchViz/ArchVizPanel.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "ArchViz/CameraSyncMode.hpp"
#include "ArchViz/CameraWake.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/HostComposite.hpp"
#include "ArchViz/Dxgi/SharedOverlaySurface.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/NavLog.hpp"
#include "ArchViz/PlanCameraMath.hpp"   // PredictPlanCamera — the `predict` mode
#include "ArchViz/PlanViewCamera.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#include <cmath>
#include <string>

// timeBeginPeriod/timeEndPeriod. Needs <windows.h> first, which the panel header
// above already pulls in; winmm is linked in CMakeLists next to this target.
#include <timeapi.h>

namespace {

// The window Archicad currently has in front, as a column in the navigation log.
const char* WindowKindName (API_WindowTypeID id)
{
    switch (id) {
        case APIWind_FloorPlanID:         return "FloorPlan";
        case APIWind_SectionID:           return "Section";
        case APIWind_ElevationID:         return "Elevation";
        case APIWind_InteriorElevationID: return "InteriorElev";
        case APIWind_DetailID:            return "Detail";
        case APIWind_WorksheetID:         return "Worksheet";
        case APIWind_3DModelID:           return "3D";
        case APIWind_LayoutID:            return "Layout";
        case APIWind_DrawingID:           return "Drawing";
        case APIWind_MasterLayoutID:      return "MasterLayout";
        default:                          return "other";
    }
}

}   // namespace

// Archicad's own 3D camera, as the viewport's starting view.
//
// ⚠️ MAIN THREAD ONLY -- it is ACAPI. Called from the posted palette handler.
//
// ⚠️ `distance` IS NOT THE ONE TO USE. API_PerspPars carries a `distance`, and
// it is the 2D PLAN distance, not the eye-to-target length; using it puts the
// camera short of where Archicad has it by exactly the height difference. The
// eye is reconstructed from pos/cameraZ instead, which is unambiguous.
//
// ⚠️ `viewCone` IS A HORIZONTAL FIELD OF VIEW IN DEGREES. Confirmed in the
// projection-overlay work. It is carried through unconverted and the render
// thread turns it into a vertical one, because that conversion needs the
// viewport's aspect ratio.
geomsrv::archviz::CameraStart ArchVizPanel::ReadArchicadCamera ()
{
    geomsrv::archviz::CameraStart start;

    API_3DProjectionInfo proj = {};
    const GSErrCode err = ACAPI_View_Get3DProjectionSets (&proj);
    if (err != NoError) {
        start.source = "ACAPI_View_Get3DProjectionSets failed (" + std::to_string ((int) err) + ")";
        return start;
    }

    if (!proj.isPersp) {
        // An axonometric view has no eye position at all -- it is a matrix, and
        // there is no single point it is "from". Framing the model is the honest
        // answer rather than inventing a camera.
        start.source = "axonometric (no camera position); the model will be framed instead";
        return start;
    }

    const API_PerspPars& p = proj.u.persp;
    start.target[0] = float (p.target.x);
    start.target[1] = float (p.target.y);
    start.target[2] = float (p.targetZ);
    start.eye[0] = float (p.pos.x);
    start.eye[1] = float (p.pos.y);
    start.eye[2] = float (p.cameraZ);
    start.viewConeDegreesHorizontal = float (p.viewCone);
    start.valid = true;
    start.source = "perspective";
    return start;
}

geomsrv::archviz::CameraStart ArchVizPanel::ReadArchicadOverlayCamera ()
{
    namespace vo = geomsrv::archviz::viewportoverlay;

    // ⚠️ THE WINDOW TYPE IS ASKED FIRST AND THE 3D READ IS THE FALLBACK, not the
    // other way round. Get3DProjectionSets succeeds over the floor plan too --
    // it answers for the 3D window whatever is in front -- so testing it first
    // would never reach the plan path at all.
    if (geomsrv::archviz::CurrentWindowIsFloorPlan ()) {
        const vo::OverlayStats stats = vo::Stats ();
        if (stats.active && stats.width >= 2 && stats.height >= 2) {
            geomsrv::archviz::CameraStart plan =
                geomsrv::archviz::ReadPlanViewCamera (stats.width, stats.height);
            if (plan.valid)
                return plan;
            // ⚠️ FALLING THROUGH TO THE 3D CAMERA WOULD BE WORSE THAN NOTHING.
            // A plan window whose mapping could not be measured is not a 3D
            // window; pushing the 3D camera would draw a perspective model over
            // a plan, which is the exact fault this dispatch exists to remove.
            // An invalid CameraStart is DROPPED by SyncCamera, so the overlay
            // simply holds its last pose and the reason is in archviz.log.
            return plan;
        }
    }
    return ReadArchicadCamera ();
}

// ---------------------------------------------------------------------------
// The navigation comparison log, Archicad's half. See the declaration in the
// header for why this is a Win32 timer and not DG's idle event, and for the
// thread rule that makes the ACAPI call below legal.
namespace {

UINT_PTR gNavTimer = 0;

// ---- the overlay camera sync -----------------------------------------------
// ⚠️ A WIN32 TIMER, NOT THE BUS, AND THAT IS THE WHOLE FIX. The first version
// drove this from Python: read the camera with one native command, push it with
// another. Both are MainThreadCommands, and while the user is dragging in the 3D
// window Archicad's main thread is inside its own modal drag loop and dispatches
// neither -- so the overlay sync test passed, and the picture only moved WHEN
// THE MOUSE BUTTON WAS RELEASED. Which is exactly what the live run reported.
//
// SetTimer(nullptr, ...) posts WM_TIMER to THIS thread's queue and
// DispatchMessage invokes the callback here, so ACAPI is legal from it for the
// same reason NavTimerProc's is -- and it keeps being dispatched inside the drag
// loop, because that loop still pumps messages.
//
// ⚠️ WM_TIMER IS STILL LOW PRIORITY: Windows generates it only when the queue is
// otherwise empty, so a very fast drag can starve it. That is a coarser sync,
// not a broken one, and NavLog's own 2603-sample run showed it firing throughout
// navigation. If starvation shows up in practice the next step is a mouse hook,
// not a shorter interval.
UINT_PTR gCameraSyncTimer = 0;

namespace nav = geomsrv::archviz::navlog;

// ---- PLAT-RE83: hide the overlay while the view is moving -------------------
//
// THE IDEA THE USER ASKED FOR, and why it is worth measuring. Following Archicad
// cannot keep up during motion -- that is structural, not a tuning problem -- so
// during a gesture the overlay is showing something WRONG. This mode shows
// nothing instead, and brings the overlay back the moment the view settles, by
// which time the camera is correct. Whether that is preferable is a question
// about how it FEELS, which only a live look answers.
//
// ⚠️ THE SETTLE DELAY IS THE WHOLE DESIGN, AND IT CUTS BOTH WAYS. Too short and
// the overlay strobes through every pause in a drag, which is worse than the lag.
// Too long and it feels unresponsive after the user has stopped.
//
// ⚠️ 120 ms WAS TOO SHORT, AND THE LOG SAYS WHY. Archicad ANIMATES its zoom:
// `PointToCoord` returns the intermediate frames of the ramp while
// `ACAPI_View_GetZoom` still reports the old settled box (2026-08-13,
// half-height stepping 20.42 -> 19.22 -> 18.44 -> 17.62 -> 17.02 against one
// unchanged zoom box). Unblanking after 120 ms lands the overlay in the MIDDLE of
// that animation, which is the reported "on zooming it reappears at a slightly
// different scale and then snaps". 250 ms clears a wheel-zoom ramp.
//
// It is also why the settle is measured from the last CAMERA CHANGE and not from
// the last input: the wheel notch stops long before Archicad has finished moving.
constexpr uint64_t kSettleMs = 250;

// What counts as "the view moved". Relative to the view's own size, so it means
// the same thing zoomed in and zoomed out; a hundredth of a view is well under a
// pixel of visible drift and well over the fit's own numerical wobble.
constexpr double kMovedFraction = 0.01;

bool CameraDiffers (const geomsrv::archviz::CameraStart& a,
                    const geomsrv::archviz::CameraStart& b)
{
    if (a.orthographic != b.orthographic)
        return true;
    if (a.orthographic) {
        const double scale = a.orthoHalfHeightMetres > 0.0 ? a.orthoHalfHeightMetres : 1.0;
        const double tolerance = scale * kMovedFraction;
        return std::fabs (a.target[0] - b.target[0]) > tolerance ||
               std::fabs (a.target[1] - b.target[1]) > tolerance ||
               std::fabs (a.orthoHalfHeightMetres - b.orthoHalfHeightMetres) > tolerance ||
               std::fabs (a.planRotationRadians - b.planRotationRadians) > 1e-4;
    }
    // Perspective: scale the tolerance by the eye-to-target distance, which is
    // this camera's equivalent of "how much of the world is on screen".
    double span = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double d = double (a.eye[axis]) - double (a.target[axis]);
        span += d * d;
    }
    span = std::sqrt (span);
    const double tolerance = (span > 0.0 ? span : 1.0) * kMovedFraction;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs (double (a.eye[axis]) - double (b.eye[axis])) > tolerance ||
            std::fabs (double (a.target[axis]) - double (b.target[axis])) > tolerance)
            return true;
    }
    return std::fabs (double (a.viewConeDegreesHorizontal) -
                      double (b.viewConeDegreesHorizontal)) > 0.01;
}

void ApplyHideOnNavigation (const geomsrv::archviz::CameraStart& camera)
{
    static geomsrv::archviz::CameraStart lastCamera;
    static bool     haveLast = false;
    static uint64_t lastChangeMs = 0;

    geomsrv::archviz::DiligentViewport& viewport = geomsrv::archviz::DiligentViewport::Get ();
    if (geomsrv::archviz::CurrentCameraSyncMode () != geomsrv::archviz::CameraSyncMode::HideOnNav) {
        // ⚠️ THE STATE IS RESET, NOT JUST IGNORED. Leaving a stale `lastCamera`
        // behind means the first tick after re-entering this mode compares
        // against wherever the view was minutes ago and blanks for no reason.
        haveLast = false;
        return;
    }

    const uint64_t nowMs = GetTickCount64 ();

    // ⚠️ THE HOOK IS THE AUTHORITY WHILE IT IS INSTALLED, and it has already set
    // the blank itself -- from inside the message pump, before Archicad acted on
    // the input (PLAT-RE75). All that is left here is the half the hook cannot
    // do: deciding the view has SETTLED, which needs a clock.
    //
    // The tear and camera-difference paths below stay as the fallback for when
    // the hook could not be installed. They are a tick late by construction; that
    // is what the hook exists to improve on, not something to fix here.
    if (geomsrv::archviz::camerawake::Installed ()) {
        if (geomsrv::archviz::camerawake::Navigating ((uint32_t) kSettleMs)) {
            if (camera.valid) {
                lastCamera = camera;
                haveLast = true;
            }
            return;   // the hook already blanked; nothing to lift yet
        }
        // ⚠️ SETTLED, BUT ONLY UNBLANK ON A GOOD READ. Lifting the blank while
        // the last sample was torn or unreadable would show the pose the overlay
        // was holding, which is the stale one -- the exact picture this mode
        // exists to avoid showing.
        if (camera.valid && !camera.viewMoving) {
            lastCamera = camera;
            haveLast = true;
            lastChangeMs = nowMs;
            viewport.SetBlanked (false);
        }
        return;
    }

    // ⚠️ THE TEAR IS CHECKED FIRST, AND BEFORE THE VALIDITY TEST, BECAUSE IT IS
    // THE ONLY SIGNAL THAT ARRIVES ON TIME.
    //
    // Comparing this camera against the previous one cannot report motion until
    // the tick AFTER it started -- by which time a frame has already been drawn
    // in the wrong place. That is the first live run's report: "does not vanish
    // immediately; the overlay jumps to some directional location and then
    // disappears". The jump WAS the detection.
    //
    // A torn sample is different in kind: it is direct evidence that Archicad
    // scrolled the view while we were reading it, so it is true on the FIRST
    // moving frame. It also survives the sample being dropped, which is exactly
    // the case that used to leave the blank off.
    //
    // ⚠️ THIS IS STILL NOT IMMEDIATE. The tear is detected during a poll, so the
    // worst case is one poll interval (~16 ms) after the motion begins. Reacting
    // to the INPUT rather than to its effect needs the wake hook, PLAT-RE75.
    if (camera.viewMoving) {
        lastChangeMs = nowMs;
        viewport.SetBlanked (true);
        if (camera.valid) {
            lastCamera = camera;
            haveLast = true;
        }
        return;
    }

    // An invalid read is not a stationary view. SyncCamera already dropped it, so
    // the overlay is holding a pose that may be stale; treating the silence as
    // "settled" would show that stale pose confidently. Hold whatever the blank
    // currently is and wait for a real answer.
    if (!camera.valid)
        return;

    if (!haveLast) {
        lastCamera = camera;
        haveLast = true;
        lastChangeMs = nowMs;
        return;
    }

    if (CameraDiffers (camera, lastCamera)) {
        lastCamera = camera;
        lastChangeMs = nowMs;
        viewport.SetBlanked (true);
        return;
    }
    if (nowMs - lastChangeMs >= kSettleMs)
        viewport.SetBlanked (false);
}

// Microseconds, because the thing being measured is a handful of milliseconds
// and GetTickCount64's 15 ms resolution cannot see it at all.
uint64_t MicrosecondsNow ()
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    if (!::QueryPerformanceFrequency (&frequency) || frequency.QuadPart == 0 ||
        !::QueryPerformanceCounter (&counter))
        return 0;
    return uint64_t (counter.QuadPart * 1000000ll / frequency.QuadPart);
}

// ---- PLAT-RE76: advance the plan camera to where the view will BE -----------
//
// ⚠️ PLAN ONLY, AND THAT IS NOT A SHORTCUT. The plan camera has four degrees of
// freedom and Archicad animates it -- the 2026-08-13 logs caught `PointToCoord`
// returning the intermediate frames of Archicad's own zoom ramp -- so its motion
// is close to exactly predictable. A perspective orbit is not: it is non-linear
// in the camera parameters and driven directly by an unpredictable hand.
// Extrapolating the 3D camera is PLAT-RE80, a separate question with its own
// answer, and quietly applying this to it would produce a swimming picture.
//
// ⚠️ THE HORIZON IS MEASURED NOW, NOT GUESSED (PLAT-RE92). It used to be a fixed
// 31 ms -- "one 15 ms poll plus one 16 ms frame" -- and that was wrong in both
// directions at once, because neither term was 15 or 16. PLAT-RE78's frame clock
// and the unthrottled poll finally gave the real numbers: Archicad redraws every
// ~19 ms while our poll lands every 23-31 ms, and the best-fit lag tracked the
// POLL interval (24-28 ms) rather than the frame time. We are one SAMPLE behind,
// not one frame, so the distance to close is the interval between samples -- and
// that interval is something this function can simply watch.
//
// A running average rather than the last gap: a single starved poll would
// otherwise throw one enormous prediction, and an overshoot spike is far more
// visible than steady lag. That asymmetry is the whole reason `predict` read as
// the worst mode by eye while measuring the best lag.
constexpr double kHorizonBlend = 0.15;
constexpr double kMinHorizonSeconds = 0.004;
constexpr double kMaxHorizonSeconds = 0.060;

geomsrv::archviz::CameraStart ApplyPrediction (const geomsrv::archviz::CameraStart& camera)
{
    static geomsrv::archviz::PlanCameraPredictor g_predictor;
    static uint64_t g_lastObservationMs = 0;
    static double   g_horizonSeconds = 0.025;   // seeded near the measured interval

    const geomsrv::archviz::CameraSyncMode mode = geomsrv::archviz::CurrentCameraSyncMode ();
    const bool predicting = (mode == geomsrv::archviz::CameraSyncMode::Predict ||
                             mode == geomsrv::archviz::CameraSyncMode::WakePredict);
    if (!predicting || !camera.orthographic || !camera.valid) {
        // ⚠️ THE PREDICTOR IS RESET, NOT JUST BYPASSED. A stale velocity from
        // minutes ago would be applied to the first frame after the mode comes
        // back, which is a jump on exactly the frame a user is watching for one.
        g_predictor = geomsrv::archviz::PlanCameraPredictor {};
        g_lastObservationMs = 0;
        return camera;
    }

    // ⚠️ QPC, NOT GetTickCount64. The interval being measured is 23-31 ms and
    // GetTickCount64 advances in 15.6 ms steps, so it would quantise every
    // sample gap to 15, 31 or 47 -- and the horizon derived from it would be
    // wrong by up to half a tick on every single frame.
    const uint64_t nowUs = MicrosecondsNow ();
    const double elapsedSeconds =
        (g_lastObservationMs == 0) ? 0.0 : double (nowUs - g_lastObservationMs) / 1000000.0;
    g_lastObservationMs = nowUs;

    // The horizon IS the inter-sample interval -- that is what the measured lag
    // turned out to track. Averaged, and only over plausible gaps: a paused run
    // or a mode switch produces a multi-second "interval" that would otherwise
    // poison the average for the next dozen frames.
    if (elapsedSeconds > kMinHorizonSeconds && elapsedSeconds < kMaxHorizonSeconds)
        g_horizonSeconds += (elapsedSeconds - g_horizonSeconds) * kHorizonBlend;

    geomsrv::archviz::PlanCameraFit observed;
    observed.valid = true;
    observed.centreX = camera.target[0];
    observed.centreY = camera.target[1];
    observed.halfHeightMetres = camera.orthoHalfHeightMetres;
    observed.rotationRadians = camera.planRotationRadians;

    const geomsrv::archviz::PlanCameraFit predicted = geomsrv::archviz::PredictPlanCamera (
        g_predictor, observed, elapsedSeconds,
        g_horizonSeconds * geomsrv::archviz::CurrentPredictionScale ());

    geomsrv::archviz::CameraStart out = camera;
    out.target[0] = float (predicted.centreX);
    out.target[1] = float (predicted.centreY);
    out.orthoHalfHeightMetres = float (predicted.halfHeightMetres);
    out.planRotationRadians = float (predicted.rotationRadians);
    return out;
}

// One poll: read Archicad's camera, publish it, keep the log honest.
//
// ⚠️ SEPARATE FROM THE TIMER PROC ON PURPOSE. `wake` mode drives this from a
// posted message instead, and the two paths must do IDENTICALLY the same work --
// otherwise a comparison between the modes measures the difference between two
// implementations rather than the difference between two wake sources.
void PollCameraOnceImpl ()
{
    if (!geomsrv::archviz::DiligentViewport::Get ().IsRunning ()) {
        ArchVizPanel::StopCameraSync ();
        return;
    }
    // ⚠️ THE *OVERLAY* READ, which dispatches on which window is in front: the
    // floor plan gets a top-down orthographic camera measured from its own
    // zoom, anything else gets the 3D window's. Calling ReadArchicadCamera
    // directly here is what made the overlay draw a perspective model over a
    // plan drawing.
    //
    // Either read drops an axonometric, a failed projection read or an
    // unmeasurable plan as `valid == false`, and SyncCamera ignores those -- so
    // a window with no camera leaves the viewport's own alone rather than
    // resetting it every tick.
    //
    // ⚠️ THE OBSERVATION AND THE PREDICTION ARE KEPT APART, and the nav log below
    // depends on it. Collapsing them into one `camera` made the ground-truth row
    // carry the PREDICTED centre in `mode=predict`, so the desync analysis
    // compared the predictor against itself: it reported the same ~30 ms lag as
    // `legacy` and attributed the predictor's own overshoot to Archicad. An
    // instrument that reads its own output is worse than no instrument, because
    // the number looks reasonable. `observed` is what Archicad said; `camera` is
    // what we draw.
    const uint64_t readStartedUs = MicrosecondsNow ();
    const geomsrv::archviz::CameraStart observed = ArchVizPanel::ReadArchicadOverlayCamera ();
    const uint64_t readUs = MicrosecondsNow () - readStartedUs;
    const geomsrv::archviz::CameraStart camera = ApplyPrediction (observed);
    geomsrv::archviz::DiligentViewport::Get ().SyncCamera (camera);

    // ⚠️ THE SAME CAMERA THE RENDERER IS GIVEN, PUBLISHED FOR THE PRESENT DETOUR
    // (PLAT-RE114). The detour runs on Archicad's render thread and may not call
    // ACAPI, so the newest pose has to be left somewhere it can read -- and it
    // must be the PREDICTED one, not the raw observation, or the reprojection
    // would hand back exactly the poll interval this ladder exists to remove.
    {
        geomsrv::archviz::dxgi::SharedOverlayPose pose;
        if (camera.valid && camera.orthographic && camera.orthoHalfHeightMetres > 0.0f) {
            pose.valid = true;
            pose.centreX = camera.target[0];
            pose.centreY = camera.target[1];
            pose.halfHeightMetres = camera.orthoHalfHeightMetres;
            pose.rotationRadians = camera.planRotationRadians;
        }
        geomsrv::archviz::dxgi::SetLatestPlanPose (pose);
    }
    ApplyHideOnNavigation (camera);

    // ⚠️ THE NAV ROW IS WRITTEN FROM *THIS* POLL, and that is the point of it
    // being here rather than in a timer of its own.
    //
    // The first matrix run (2026-08-13) had TWO independent ACAPI polls going at
    // once: this one, driving the picture, and NavTimerProc, measuring a second
    // read of the same thing. So the file described a poll that was not the poll
    // under test, at double the ACAPI cost -- and the user reported Archicad's
    // own zoom going rough while it ran. Logging what THIS tick actually got
    // measures the thing that actually moves the overlay, for free.
    //
    // NavLog is off by default and throttles itself, so this costs one atomic
    // read when nobody is measuring.
    //
    // `observed`, NEVER `camera` -- see the note above the read.
    if (nav::IsRunning ()) {
        if (!observed.valid) {
            nav::LogArchicadFailure (observed.orthographic ? "FloorPlan" : "3D", observed.source);
        } else if (observed.orthographic) {
            const double centre[2] = { observed.target[0], observed.target[1] };
            nav::LogArchicadPlan ("FloorPlan", centre, observed.orthoHalfHeightMetres,
                                  observed.planRotationRadians, readUs);
        } else {
            // `distance` and `azimuth` are 0 here, unlike NavTimerProc's rows:
            // CameraStart does not carry them and both are derivable from eye and
            // target. The mode column already says which reader produced the row.
            const double eye[3] = { observed.eye[0], observed.eye[1], observed.eye[2] };
            const double tgt[3] = { observed.target[0], observed.target[1], observed.target[2] };
            nav::LogArchicadPersp ("3D", eye, tgt, 0.0, 0.0, 0.0,
                                   observed.viewConeDegreesHorizontal);
        }
        // The Present hook's ring drains from here because this is the only
        // main-thread heartbeat that is already running while a hookdiag run is
        // under way. No-op unless that hook is installed.
        geomsrv::archviz::dxgi::FlushPresentLogIfFilling ();
    }

    // ⚠️ OUTSIDE THE NAV-LOG BLOCK, and it was inside it for one run. Which swap
    // chain is Archicad's cannot be known until enough frames have gone through
    // the hook to tell it from anything else that redraws -- but `hookdraw`
    // arms and looks for its squares BEFORE the nav log is switched on, so
    // nominating the target only while logging meant it was never nominated at
    // all. The run then reported "no chain identified" while its own counters
    // showed 706 of Archicad's frames going past. No-op unless `hookdraw` is
    // armed and the target is still unset.
    geomsrv::archviz::dxgi::ChooseMarkerTargetIfUnset ();
    // The self-healing half of hookdraw: a compositor that stops while our own
    // overlay window is hidden leaves the user with an empty screen, and a
    // CANCELLED command cannot fix it -- after a Stop the bus refuses the very
    // calls its `finally` block would make. So the restore lives here.
    geomsrv::archviz::dxgi::WatchHostComposite ();
}

void CALLBACK CameraSyncTimerProc (HWND, UINT, UINT_PTR, DWORD)
{
    PollCameraOnceImpl ();
}


void CALLBACK NavTimerProc (HWND, UINT, UINT_PTR, DWORD)
{
    namespace nav = geomsrv::archviz::navlog;
    if (!nav::IsRunning ())
        return;

    // ⚠️ WHICH WINDOW IS FRONT IS PART OF THE MEASUREMENT, NOT A GUARD.
    // Get3DProjectionSets returns the 3D window's SETTINGS whatever is frontmost
    // (LayoutCommands.cpp carries the same warning, and it is plan §23's G11) —
    // so the answer is logged either way and the column says which it was. A
    // filter here would silently drop exactly the rows that prove the point.
    API_WindowInfo   info = {};
    const char*      kind = "?";
    if (ACAPI_Window_GetCurrentWindow (&info) == NoError)
        kind = WindowKindName (info.typeID);

    API_3DProjectionInfo proj = {};
    const GSErrCode      err  = ACAPI_View_Get3DProjectionSets (&proj);
    if (err != NoError) {
        nav::LogArchicadFailure (kind, "ACAPI_View_Get3DProjectionSets err=" + std::to_string ((int) err));
        return;
    }

    if (proj.isPersp) {
        const API_PerspPars& p = proj.u.persp;
        // ⚠️ THE EYE IS pos.x/pos.y/cameraZ AND THE TARGET IS target.x/target.y/
        // targetZ. Archicad splits each into a 2D coord plus a separate Z; there
        // is no API_Coord3D here, and reading pos as if it were one silently
        // drops the height.
        const double eye[3] = { p.pos.x,    p.pos.y,    p.cameraZ };
        const double tgt[3] = { p.target.x, p.target.y, p.targetZ };
        // ⚠️ NO RADIAN CONVERSION. `azimuth` and `viewCone` are ALREADY IN
        // DEGREES, measured over 2603 samples 2026-08-06: viewCone came back a
        // flat 75.000 (Archicad's default cone, in degrees; 75 radians is not a
        // thing) and azimuth stayed inside [0,360). The DevKit's comments say
        // only "rotation angle" and "angle of the camera view cone" and do not
        // state a unit, which is how a conversion got added here — against a
        // fact this repo had ALREADY recorded (Camera.hpp, and plan §6.4:
        // "viewCone is HORIZONTAL and in degrees, confirmed"). Grep before
        // converting.
        // `rollAngle` was 0.000 in every sample, so its unit is UNVERIFIED; it
        // is passed through raw on the assumption that one struct does not mix
        // units. Do not treat that as measured.
        nav::LogArchicadPersp (kind, eye, tgt, p.distance,
                               p.azimuth, p.rollAngle, p.viewCone);
    } else {
        const API_AxonoPars& a = proj.u.axono;
        double tm[12] = {};
        for (int i = 0; i < 12; ++i)
            tm[i] = a.tranmat.tmx[i];
        // Raw, for the same reason as the perspective branch above — except
        // that the axono azimuth has NOT been measured (no axono view has been
        // logged yet; every sample so far was perspective). Treated as degrees
        // by analogy, which is a guess and is labelled as one here rather than
        // in whatever reads the file.
        nav::LogArchicadAxono (kind, a.azimuth, (int32_t) a.projMod, tm);
    }
}

// ⚠️ WITHOUT THIS, THE INTERVAL ARGUMENT IS LARGELY FICTION. Windows' default
// timer granularity is 15.625 ms, and `SetTimer` cannot fire between ticks: a
// 15 ms request expires just before the next boundary, so any pump delay of more
// than ~0.6 ms pushes it to the tick AFTER, i.e. 31.25 ms. The 2026-08-13 matrix
// measured exactly that -- a poll asked for 66 Hz, delivering a median 30 ms at
// idle and worse under drag, which put a ~30 ms floor under every desync number
// in the run no matter what the renderer did.
//
// `timeBeginPeriod (1)` drops the granularity to ~1 ms. Since Windows 10 2004 it
// is scoped to the calling process, so this does not slow the rest of the
// machine down; it does cost power, which is why it is held only while the
// overlay's camera sync is actually armed and released the moment it stops.
//
// ⚠️ EVERY BEGIN NEEDS ITS END. The period is reference-counted per process, so a
// leaked call leaves Archicad in a high-resolution timer state for the rest of
// its lifetime. `g_timerPeriodHeld` makes the pairing impossible to get wrong
// across the repeated Start/Stop that a mode switch performs.
bool g_timerPeriodHeld = false;

void HoldTimerResolution ()
{
    if (!g_timerPeriodHeld && ::timeBeginPeriod (1) == TIMERR_NOERROR)
        g_timerPeriodHeld = true;
}

void ReleaseTimerResolution ()
{
    if (g_timerPeriodHeld) {
        ::timeEndPeriod (1);
        g_timerPeriodHeld = false;
    }
}

}   // namespace

void ArchVizPanel::PollCameraOnce ()
{
    PollCameraOnceImpl ();
}

bool ArchVizPanel::StartCameraSync (uint32_t intervalMs)
{
    StopCameraSync ();
    if (intervalMs < 10)
        intervalMs = 10;
    HoldTimerResolution ();
    gCameraSyncTimer = ::SetTimer (nullptr, 0, intervalMs, CameraSyncTimerProc);
    if (gCameraSyncTimer == 0) {
        ReleaseTimerResolution ();
        geomsrv::archviz::ArchVizLog ("overlay camera sync: SetTimer failed; the Diligent "
                                      "viewport will keep its own camera and will NOT follow "
                                      "Archicad's 3D window");
        return false;
    }
    geomsrv::archviz::ArchVizLog ("overlay camera sync: following Archicad's 3D window every " +
                                  std::to_string (intervalMs) + " ms");
    return true;
}

void ArchVizPanel::StopCameraSync ()
{
    if (gCameraSyncTimer != 0) {
        ::KillTimer (nullptr, gCameraSyncTimer);
        gCameraSyncTimer = 0;
    }
    ReleaseTimerResolution ();
}

// `sampler` decides whether this opens its OWN ACAPI poll.
//
// ⚠️ THE MATRIX PASSES FALSE, AND THAT IS THE CORRECT MEASUREMENT, not a saving.
// With a sampler there are two polls: the camera sync (which moves the overlay)
// and NavTimerProc (which does not). Rows then describe the second one, while
// every verdict is about the first -- and both read ACAPI on the main thread, at
// a cost the 2026-08-13 run reported as Archicad's own zoom going rough.
// Sampler-less, the only rows come from the sync tick, so the file describes the
// poll actually under test.
//
// It stays available because the standalone comparison (viewer camera vs
// Archicad camera, with no overlay involved) genuinely needs an independent
// source, and that is what NavLog was built for.
bool ArchVizPanel::StartNavLog (uint32_t intervalMs, bool sampler)
{
    StopNavLog ();
    if (intervalMs < 10)
        intervalMs = 10;

    geomsrv::archviz::navlog::Start (intervalMs);
    if (!sampler) {
        geomsrv::archviz::ArchVizLog ("nav log: no sampler -- rows come from the camera sync "
                                      "poll itself, which is the poll under test");
        return true;
    }
    // nullptr window: the WM_TIMER is posted to THIS thread's queue and
    // DispatchMessage invokes the callback here, so no window ownership is
    // involved and nothing needs subclassing. Archicad pumps the queue.
    gNavTimer = ::SetTimer (nullptr, 0, intervalMs, NavTimerProc);
    if (gNavTimer == 0) {
        geomsrv::archviz::navlog::Stop ();
        geomsrv::archviz::ArchVizLog ("nav log: SetTimer failed; Archicad's camera cannot be "
                                      "sampled and only the viewer's half would have been "
                                      "written, which is not a comparison. Not started.");
        return false;
    }
    return true;
}

void ArchVizPanel::StopNavLog ()
{
    if (gNavTimer != 0) {
        ::KillTimer (nullptr, gNavTimer);
        gNavTimer = 0;
    }
    geomsrv::archviz::navlog::Stop ();
}
