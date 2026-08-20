// ArchViz/CameraSyncMode -- see the header for why the mechanism is a runtime
// mode. MAIN THREAD ONLY.

#include "ArchViz/CameraSyncMode.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/CameraWake.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/HostComposite.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExperimentGuard.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

namespace geomsrv {
namespace archviz {

namespace {

CameraSyncMode g_mode = CameraSyncMode::Off;
uint32_t       g_intervalMs = 33;
double         g_predictionScale = 1.0;

// Disarm whatever `g_mode` currently is. ⚠️ IT SWITCHES ON THE OLD MODE, not on
// what is being armed next: each mechanism owns a different resource (a timer, a
// hook, a detour) and leaving one of them installed while another is armed is
// the failure this whole file is meant to make impossible.
void TearDownCurrent ()
{
    switch (g_mode) {
        case CameraSyncMode::Off:
            break;
        case CameraSyncMode::Legacy:
            ArchVizPanel::StopCameraSync ();
            break;
        case CameraSyncMode::HideOnNav:
            ArchVizPanel::StopCameraSync ();
            camerawake::Remove ();
            // ⚠️ THE BLANK MUST BE LIFTED ON THE WAY OUT. It lives on the
            // viewport, not on the timer, so leaving this mode mid-navigation
            // would strand an invisible overlay with no switch left to fix it.
            DiligentViewport::Get ().SetBlanked (false);
            break;
        case CameraSyncMode::Predict:
            ArchVizPanel::StopCameraSync ();
            break;
        case CameraSyncMode::Wake:
        case CameraSyncMode::WakePredict:
            ArchVizPanel::StopCameraSync ();
            // Remove() clears the callback and destroys the window; the blank is
            // lifted for the same reason as hideonnav's.
            camerawake::Remove ();
            DiligentViewport::Get ().SetBlanked (false);
            break;
        case CameraSyncMode::HookDiag:
            ArchVizPanel::StopCameraSync ();
            // ⚠️ THE RING IS FLUSHED BEFORE THE HOOK COMES OUT, not after. Once
            // the detour is gone the ring stops being written but it is still
            // the only copy of the frame clock this session recorded, and
            // tearing down without flushing throws away the entire measurement
            // the mode existed to take.
            dxgi::FlushPresentLog ();
            dxgi::RemovePresentHook ();
            break;
        case CameraSyncMode::HookDraw:
            ArchVizPanel::StopCameraSync ();
            // ⚠️ THE MARKER STOPS BEFORE THE HOOK COMES OUT. Disabling it is a
            // single atomic store, so any present already inside the detour
            // either drew or did not; removing the hook first would leave a
            // window in which the detour is gone but the marker still believes
            // it is live, and the next arm would start with a stale target.
            dxgi::SetMarkerEnabled (false);
            dxgi::SetHostCompositeEnabled (false);
            // ⚠️ THE WINDOW COMES BACK BEFORE ANYTHING ELSE. Every other exit
            // from this mode -- a cancelled run, a failed arm, add-on shutdown --
            // goes through here, and one that forgot would leave the user with a
            // viewport that is running, presenting and permanently invisible,
            // with no switch left to bring it back.
            viewportoverlay::SetVisible (true);
            dxgi::FlushPresentLog ();
            dxgi::RemovePresentHook ();
            break;
    }
    // ⚠️ THE BREADCRUMB GOES WITH THE MECHANISM. It was dropped only on an arm
    // failure or at add-on shutdown, so a clean switch from `hookdraw` to
    // `legacy` left `EXPERIMENT_ARMED` on disk -- and an unrelated crash hours
    // later would then refuse every experimental mode next session and blame a
    // hook that had not been installed since.
    if (IsExperimental (g_mode))
        experimentguard::Disarm ();
    g_mode = CameraSyncMode::Off;
}

// The step that will implement a mode, so the refusal says what is missing
// rather than "unsupported".
const char* NotYetBuilt (CameraSyncMode mode)
{
    (void) mode;
    return nullptr;
}

}   // namespace

bool ParseCameraSyncMode (const std::string& name, CameraSyncMode& mode)
{
    if (name == "off")      { mode = CameraSyncMode::Off;      return true; }
    if (name == "legacy")   { mode = CameraSyncMode::Legacy;   return true; }
    if (name == "hideonnav"){ mode = CameraSyncMode::HideOnNav;return true; }
    if (name == "wake")     { mode = CameraSyncMode::Wake;     return true; }
    if (name == "predict")  { mode = CameraSyncMode::Predict;  return true; }
    if (name == "wakepredict") { mode = CameraSyncMode::WakePredict; return true; }
    if (name == "hookdiag") { mode = CameraSyncMode::HookDiag; return true; }
    if (name == "hookdraw") { mode = CameraSyncMode::HookDraw; return true; }
    return false;
}

const char* CameraSyncModeName (CameraSyncMode mode)
{
    switch (mode) {
        case CameraSyncMode::Off:      return "off";
        case CameraSyncMode::Legacy:   return "legacy";
        case CameraSyncMode::HideOnNav:return "hideonnav";
        case CameraSyncMode::Wake:     return "wake";
        case CameraSyncMode::Predict:  return "predict";
        case CameraSyncMode::WakePredict: return "wakepredict";
        case CameraSyncMode::HookDiag: return "hookdiag";
        case CameraSyncMode::HookDraw: return "hookdraw";
    }
    return "?";
}

bool IsExperimental (CameraSyncMode mode)
{
    // ⚠️ `HideOnNav` COUNTS BECAUSE IT INSTALLS THE WAKE HOOK (PLAT-RE75). It
    // began as a pure viewport flag and was not experimental; giving it the hook
    // -- the only way to react to navigation on the input rather than on its
    // consequence -- gave it the crash-on-unload hazard too. A Windows hook left
    // installed when the DLL goes away is Windows calling into freed code,
    // exactly like a stale detour, so it earns a breadcrumb even though it is far
    // cheaper than the DXGI modes.
    return mode == CameraSyncMode::HideOnNav || mode == CameraSyncMode::Wake ||
           mode == CameraSyncMode::WakePredict ||
           mode == CameraSyncMode::HookDiag || mode == CameraSyncMode::HookDraw;
}

bool SetCameraSyncMode (CameraSyncMode mode, uint32_t intervalMs, double predictionScale,
                        std::string& error)
{
    if (intervalMs < 10)
        intervalMs = 10;
    // Clamped, not refused: a scale outside this range is a typo, and refusing
    // the whole mode switch over one would strand the run.
    if (!(predictionScale >= 0.0))   // also catches NaN
        predictionScale = 1.0;
    if (predictionScale > 4.0)
        predictionScale = 4.0;
    g_predictionScale = predictionScale;

    // ---- everything that can refuse happens BEFORE anything is torn down ----
    if (const char* missing = NotYetBuilt (mode)) {
        error = std::string (missing) + "; the current mode ('" + CameraSyncModeName (g_mode) +
                "') is unchanged";
        return false;
    }

    if (IsExperimental (mode) && experimentguard::Blocked ()) {
        error = experimentguard::WhyBlocked () + " -- the current mode ('" +
                CameraSyncModeName (g_mode) + "') is unchanged";
        return false;
    }

    if (mode != CameraSyncMode::Off && !DiligentViewport::Get ().IsRunning ()) {
        error = "no Diligent viewport or overlay is running, so there is nothing to sync; "
                "the current mode ('" + std::string (CameraSyncModeName (g_mode)) +
                "') is unchanged";
        return false;
    }

    // ---- from here the switch is committed --------------------------------
    TearDownCurrent ();

    if (mode == CameraSyncMode::Off) {
        g_intervalMs = intervalMs;
        ArchVizLog ("camera sync mode: off");
        return true;
    }

    if (IsExperimental (mode) && !experimentguard::Arm (CameraSyncModeName (mode), error)) {
        // The guard refused to leave a breadcrumb, so the mechanism must not be
        // installed. We are already torn down, which is the safe resting place.
        ArchVizLog ("camera sync mode: refused '" + std::string (CameraSyncModeName (mode)) +
                    "' -- " + error + "; now off");
        return false;
    }

    bool armed = false;
    switch (mode) {
        case CameraSyncMode::Legacy:
        case CameraSyncMode::Predict:
            // Same timer. `predict` differs only in what the tick does with the
            // camera it read -- it advances it to where the view will be at the
            // overlay's next present instead of pushing where the view WAS.
            // No hook, no window, nothing to leave behind: that is why it is not
            // experimental while `hideonnav` is.
            armed = ArchVizPanel::StartCameraSync (intervalMs);
            break;
        case CameraSyncMode::HideOnNav:
            // The timer still reads the camera and decides when the view has
            // SETTLED -- a timed judgement, and the hook has no clock. The hook's
            // job is the other half: blanking the instant an input arrives,
            // before Archicad has moved anything.
            //
            // ⚠️ THE HOOK GOES FIRST. Arming the timer and then failing to
            // install the hook would leave a half-built mode running, and its
            // symptom -- blanking a tick late -- is precisely the bug this mode
            // was extended to fix, so it would look like the fix simply did not
            // work.
            camerawake::SetBlankOnInput (true);
            armed = camerawake::Install (error) && ArchVizPanel::StartCameraSync (intervalMs);
            if (!armed)
                camerawake::Remove ();
            break;
        case CameraSyncMode::WakePredict:
        case CameraSyncMode::Wake:
            // `hideonnav` plus the other half: the hook now also POSTS a camera
            // read, so a sample lands at input priority instead of waiting for
            // WM_TIMER -- which Windows serves last, and which the 2026-08-13
            // runs measured at 24-41 ms under drag against a 15 ms request.
            //
            // ⚠️ THE CALLBACK IS SET BEFORE THE HOOK IS INSTALLED. The hook can
            // fire on the very next message, and RequestPoll does nothing
            // without a callback -- so setting it afterwards would silently drop
            // the first inputs of the mode.
            //
            // The timer stays armed underneath as the heartbeat: a zoom
            // animation continues after the wheel notch that caused it, and a
            // resize moves the camera with no input at all.
            camerawake::SetPollCallback (&ArchVizPanel::PollCameraOnce);
            armed = camerawake::Install (error) && ArchVizPanel::StartCameraSync (intervalMs);
            if (!armed)
                camerawake::Remove ();
            break;
        case CameraSyncMode::HookDiag:
            // ⚠️ DIAGNOSTIC ONLY. This mode changes NOTHING about how the overlay
            // is drawn -- the camera sync runs exactly as `legacy` does. The hook
            // only records when frames go out, which is the one thing no other
            // rung can see and the thing PLAT-RE79 needs before it is worth
            // attempting.
            //
            // The hook goes first for the same reason as hideonnav's: a half-
            // built experimental mode is worse than a refused one.
            armed = dxgi::InstallPresentHook (error) &&
                    ArchVizPanel::StartCameraSync (intervalMs);
            if (!armed)
                dxgi::RemovePresentHook ();
            break;
        case CameraSyncMode::HookDraw:
            // ⚠️ PHASE 3 IS A GATE, NOT A FEATURE. This mode draws ONE FIXED
            // SQUARE into Archicad's back buffer and syncs the camera exactly as
            // `legacy` does. It answers one question -- can a pixel be put into
            // Archicad's own frame, reliably, without destabilising it -- and if
            // the square cannot be made to sit still, the host-hook path stops
            // here instead of after weeks of renderer work. See HookMarker.hpp.
            //
            // The target chain is NOT chosen here: identifying it takes a second
            // of frames, and blocking the main thread that long inside the arm
            // call would stall Archicad's UI. The camera tick nominates it once
            // enough frames have been seen, so the square appears a moment after
            // the mode is armed rather than instantly. That delay is expected.
            //
            // ⚠️ PHASE 4 NOW COMPOSITES THE REAL OVERLAY. The marker squares
            // stay armed and draw ONLY until the compositor reports ready, so
            // they remain the "the hook is alive, the overlay is not arriving"
            // signal and stop the moment there is something better to look at.
            dxgi::SetMarkerEnabled (true);
            dxgi::SetHostCompositeEnabled (true);
            armed = dxgi::InstallPresentHook (error) &&
                    ArchVizPanel::StartCameraSync (intervalMs);
            // ⚠️ OUR OWN OVERLAY WINDOW GOES AWAY WHILE THIS MODE IS ON. It keeps
            // rendering and presenting -- the mirror copies those frames -- but
            // leaving it VISIBLE would draw the overlay twice, a few
            // milliseconds apart, which is precisely the ghosting this mode
            // exists to remove. If the compositor then fails, the user sees the
            // phase-3 marker squares and no overlay, which is an unambiguous
            // state rather than a confusing one.
            if (armed)
                viewportoverlay::SetVisible (false);
            if (!armed) {
                dxgi::SetHostCompositeEnabled (false);
                dxgi::SetMarkerEnabled (false);
                dxgi::RemovePresentHook ();
            }
            break;
        default:
            // Unreachable: NotYetBuilt already refused every other mode above.
            break;
    }

    if (!armed) {
        if (IsExperimental (mode))
            experimentguard::Disarm ();
        error = "arming camera sync mode '" + std::string (CameraSyncModeName (mode)) +
                "' failed; sync is now off";
        ArchVizLog ("camera sync mode: " + error);
        return false;
    }

    g_mode = mode;
    g_intervalMs = intervalMs;
    ArchVizLog ("camera sync mode: " + std::string (CameraSyncModeName (mode)) + " at " +
                std::to_string (intervalMs) + " ms");
    return true;
}

CameraSyncMode CurrentCameraSyncMode ()
{
    return g_mode;
}

double CurrentPredictionScale ()
{
    return g_predictionScale;
}

uint32_t CurrentCameraSyncIntervalMs ()
{
    return g_intervalMs;
}

void ShutDownCameraSync ()
{
    TearDownCurrent ();
    // ⚠️ UNCONDITIONAL, not only when the old mode was experimental. This runs on
    // teardown paths where the mode may already have been lost, and a breadcrumb
    // that outlives a clean exit blocks the NEXT session for no reason.
    experimentguard::Disarm ();
}

}   // namespace archviz
}   // namespace geomsrv
