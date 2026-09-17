// ArchViz/CameraSyncMode -- see the header for why the mechanism is a runtime
// mode. MAIN THREAD ONLY.

#include "ArchViz/CameraSyncMode.hpp"

#include "ArchViz/AutoOrbit.hpp"

#include "ArchViz/ArchVizLog.hpp" // ArchVizLog
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/CameraWake.hpp"
#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/HostComposite.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExperimentGuard.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

namespace geomsrv {
namespace archviz {

namespace {

CameraSyncMode g_mode = CameraSyncMode::Off;
uint32_t g_intervalMs = 33;
double g_predictionScale = 1.0;
// ⚠️ OFF BY DEFAULT (user, 2026-08-28), and it was on for one build. The argument
// for on -- a dependent reading a mid-drag frame is shown a pose that is wrong by
// construction -- is still true, but it was outweighed in practice: the blank
// fires on gestures that are not navigation at all. A rubber-band selection is a
// left drag STARTING OVER THE VIEW, so even the input filter that fixed the
// palette-drag case (PLAT-RE145) cannot tell it from a pan without knowing which
// Archicad tool is active, which is not something this layer can see. An overlay
// that vanishes while the user drags a selection box is worse than a slightly
// stale one. `hideonnav` and the `hideOnNav` parameter both still reach it.
bool g_hideOnNav = false;
// Whether `hookdiag` also installs the GPU-state discovery hooks. See the
// header: a switch on hookdiag rather than a ninth mode, refused everywhere else
// rather than ignored.
bool g_gpuState = false;

// Does this mode install the wake hook? The hook is what lets `hideOnNav` blank
// on the INPUT rather than on its consequence, and it is also what makes a mode
// experimental -- so the two questions have exactly one answer between them and
// it is written down once.
bool InstallsWakeHook (CameraSyncMode mode)
{
    return mode == CameraSyncMode::HideOnNav || mode == CameraSyncMode::Wake || mode == CameraSyncMode::WakePredict ||
           mode == CameraSyncMode::HookDraw;
}

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
        case CameraSyncMode::Predict:
            ArchVizPanel::StopCameraSync ();
            // ⚠️ THE BLANK MUST BE LIFTED ON THE WAY OUT, and these two modes had
            // no reason to do it until `hideOnNav` stopped being a mode of its
            // own. It lives on the viewport, not on the timer, so leaving a
            // blanking mode mid-navigation would strand an invisible overlay with
            // no switch left to fix it.
            DiligentViewport::Get ().SetBlanked (false);
            break;
        case CameraSyncMode::HideOnNav:
            ArchVizPanel::StopCameraSync ();
            camerawake::Remove ();
            DiligentViewport::Get ().SetBlanked (false);
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
            // ⚠️ THE RINGS ARE FLUSHED BEFORE THE HOOKS COME OUT, not after. Once
            // a detour is gone its ring stops being written but it is still
            // the only copy of what this session recorded, and tearing down
            // without flushing throws away the entire measurement the mode
            // existed to take.
            //
            // ⚠️ AND THE CONTEXT HOOK COMES OUT BEFORE THE PRESENT HOOK. The
            // frame boundary the capture closes on is the Present detour, so
            // removing Present first would leave the context hook recording
            // into a frame that never ends -- and its own removal drains its
            // detours, which is a wait that must not happen with a second
            // detour still feeding it.
            dxgi::renderstate::FlushFrameLog ();
            dxgi::FlushContextLog ();
            // ⚠️ THE WANT IS CLEARED BEFORE THE REMOVE. It is what lets the
            // present detour discover, and leaving it set while the table is
            // being restored would let a discovery land against a hook that is
            // on its way out.
            dxgi::SetContextHookWanted (false);
            dxgi::RemoveContextHook ();
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
            // ⚠️ THIS MODE OWNS THE WAKE HOOK NOW (PLAT-RE116) and must give it
            // back, for the same reason `wakepredict` does: a hook left installed
            // when the DLL unloads is Windows calling into freed code.
            camerawake::Remove ();
            DiligentViewport::Get ().SetBlanked (false);
            break;
    }
    // ⚠️ THE ORBIT STOPS ON EVERY TEARDOWN, WHICHEVER MODE WAS ARMED, and that
    // is what makes `CameraSyncReset` the way out of it. A diagnostic that turns
    // the camera and is then CANCELLED cannot put the view back itself: after a
    // Stop the bus refuses the very calls its `finally` block would make. So the
    // restore lives here, on the path every exit already goes through, exactly
    // as `viewportoverlay::SetVisible (true)` does for `hookdraw`. No-op unless
    // something armed it, and it writes the saved projection back verbatim.
    // ⚠️ THE INJECTED TRIANGLE STOPS ON EVERY TEARDOWN, and its device objects go
    // with it. It draws into Archicad's own scene target; a cancelled run must
    // not be able to leave that running, and nothing it created may outlive
    // Archicad's device.
    dxgi::injection::SetEnabled (false);
    dxgi::injection::Shutdown ();

    // ⚠️ THE CENSUS GOES THE SAME WAY, for the same reason. It draws nothing, but
    // it owns staging buffers on Archicad's device and nothing here may outlive
    // that device.
    dxgi::census::SetEnabled (false);
    dxgi::census::Shutdown ();

    autoorbit::Stop ();

    // ⚠️ THE BREADCRUMB GOES WITH THE MECHANISM. It was dropped only on an arm
    // failure or at add-on shutdown, so a clean switch from `hookdraw` to
    // `legacy` left `EXPERIMENT_ARMED` on disk -- and an unrelated crash hours
    // later would then refuse every experimental mode next session and blame a
    // hook that had not been installed since.
    if (IsExperimental (g_mode))
        experimentguard::Disarm ();
    g_mode = CameraSyncMode::Off;
    // ⚠️ CLEARED WITH THE MECHANISM, like the breadcrumb above. A switch left
    // set after the hooks it named have been removed makes the diagnostic report
    // a capture that is not running.
    g_gpuState = false;
}

// The step that will implement a mode, so the refusal says what is missing
// rather than "unsupported".
const char* NotYetBuilt (CameraSyncMode mode)
{
    (void) mode;
    return nullptr;
}

} // namespace

bool ParseCameraSyncMode (const std::string& name, CameraSyncMode& mode)
{
    if (name == "off") {
        mode = CameraSyncMode::Off;
        return true;
    }
    if (name == "legacy") {
        mode = CameraSyncMode::Legacy;
        return true;
    }
    if (name == "hideonnav") {
        mode = CameraSyncMode::HideOnNav;
        return true;
    }
    if (name == "wake") {
        mode = CameraSyncMode::Wake;
        return true;
    }
    if (name == "predict") {
        mode = CameraSyncMode::Predict;
        return true;
    }
    if (name == "wakepredict") {
        mode = CameraSyncMode::WakePredict;
        return true;
    }
    if (name == "hookdiag") {
        mode = CameraSyncMode::HookDiag;
        return true;
    }
    if (name == "hookdraw") {
        mode = CameraSyncMode::HookDraw;
        return true;
    }
    return false;
}

const char* CameraSyncModeName (CameraSyncMode mode)
{
    switch (mode) {
        case CameraSyncMode::Off:
            return "off";
        case CameraSyncMode::Legacy:
            return "legacy";
        case CameraSyncMode::HideOnNav:
            return "hideonnav";
        case CameraSyncMode::Wake:
            return "wake";
        case CameraSyncMode::Predict:
            return "predict";
        case CameraSyncMode::WakePredict:
            return "wakepredict";
        case CameraSyncMode::HookDiag:
            return "hookdiag";
        case CameraSyncMode::HookDraw:
            return "hookdraw";
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
    //
    // ⚠️ IT IS A FUNCTION OF THE MODE ALONE, never of `hideOnNav`. Blanking is a
    // viewport flag and carries no unload hazard of its own; what carries it is
    // the hook, and only a mode decides whether one is installed. Letting a
    // switch move a mode in and out of the guard would mean the breadcrumb on
    // disk no longer named what was armed.
    return InstallsWakeHook (mode) || mode == CameraSyncMode::HookDiag;
}

bool SetCameraSyncMode (CameraSyncMode mode, uint32_t intervalMs, double predictionScale, bool hideOnNav, bool gpuState,
                        std::string& error)
{
    if (intervalMs < 10)
        intervalMs = 10;
    // Clamped, not refused: a scale outside this range is a typo, and refusing
    // the whole mode switch over one would strand the run.
    if (!(predictionScale >= 0.0)) // also catches NaN
        predictionScale = 1.0;
    if (predictionScale > 4.0)
        predictionScale = 4.0;
    g_predictionScale = predictionScale;

    // ---- everything that can refuse happens BEFORE anything is torn down ----
    if (const char* missing = NotYetBuilt (mode)) {
        error = std::string (missing) + "; the current mode ('" + CameraSyncModeName (g_mode) + "') is unchanged";
        return false;
    }

    // ⚠️ CLEARED FOR EVERY OTHER MODE, NEVER REFUSED HERE, AND THE FIRST LIVE RUN
    // IS WHY. This used to refuse `gpuState` on any mode but `hookdiag`, and the
    // bus command defaults the argument to whatever is currently set -- so once a
    // `hookdiag` run had turned it on, `SetCameraSyncMode {mode: "legacy"}`
    // inherited `gpuState: true` and was REFUSED. That is the teardown path. It
    // made the mode undisarmable through the normal route and broke
    // `CameraSyncReset`, which sends exactly that call, at the one moment either
    // is needed. A switch that is meaningless for a mode is cleared by choosing
    // that mode; refusing an EXPLICIT request is the command layer's job, where
    // "the caller asked for this" can still be told from "the caller said
    // nothing".
    if (mode != CameraSyncMode::HookDiag)
        gpuState = false;

    if (IsExperimental (mode) && experimentguard::Blocked ()) {
        error = experimentguard::WhyBlocked () + " -- the current mode ('" + CameraSyncModeName (g_mode) +
                "') is unchanged";
        return false;
    }

    // ⚠️ EVERY MODE BUT `hookdiag` MOVES THE PORTABLE VIEWPORT'S
    // CAMERA, so with no viewport there is genuinely nothing to sync and refusing
    // is right. `hookdiag` IS NOT ONE OF THOSE. Its hooks read Archicad's OWN
    // D3D context, through a detour on Archicad's OWN swap chain, to drive the
    // INJECTED overlay -- which has no Diligent viewport, no surface and no
    // camera of its own to move.
    //
    // ⚠️ THIS GUARD IS WHY THE INJECTED OVERLAY COULD NEVER START ON
    // ITS OWN. `InjectedOverlayRuntime::Start` arms `hookdiag` and was refused
    // with "nothing to sync" on every attempt, so the only way to reach the
    // injected path was to open the portable overlay FIRST -- which is exactly
    // the lifecycle coupling stage 9d was written to remove, surviving in the one
    // place nobody had looked. It also explains why every successful diagnostic
    // run in this series began with the user opening the Tapioca Overlay by hand:
    // that was not a convenience, it was mandatory.
    if (mode != CameraSyncMode::Off && mode != CameraSyncMode::HookDiag && !DiligentViewport::Get ().IsRunning ()) {
        error = "no Diligent viewport or overlay is running, so there is nothing to sync; "
                "the current mode ('" +
                std::string (CameraSyncModeName (g_mode)) + "') is unchanged";
        return false;
    }

    // ---- from here the switch is committed --------------------------------
    TearDownCurrent ();

    // ⚠️ AFTER THE TEARDOWN, NEVER BEFORE. `TearDownCurrent` lifts the blank the
    // OLD configuration left behind, and it decides whether to by looking at the
    // old mode -- writing the new switch over it first would be indistinguishable
    // from the caller having asked for the old one.
    //
    // `hideonnav` pins it rather than reading the argument: the name predates the
    // switch and every existing caller sends it meaning exactly "blank".
    g_hideOnNav = (mode == CameraSyncMode::HideOnNav) ? true : hideOnNav;
    g_gpuState = gpuState;

    if (mode == CameraSyncMode::Off) {
        g_intervalMs = intervalMs;
        ArchVizLog ("camera sync mode: off");
        return true;
    }

    if (IsExperimental (mode) && !experimentguard::Arm (CameraSyncModeName (mode), error)) {
        // The guard refused to leave a breadcrumb, so the mechanism must not be
        // installed. We are already torn down, which is the safe resting place.
        ArchVizLog ("camera sync mode: refused '" + std::string (CameraSyncModeName (mode)) + "' -- " + error +
                    "; now off");
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
            // `g_hideOnNav`, not a literal `true` -- the line above pinned it for
            // this mode, and hard-coding it here as well would leave two places
            // that have to agree about one fact.
            //
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
            camerawake::SetBlankOnInput (g_hideOnNav);
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
            //
            // ⚠️ THESE MODES MAY NOW BLANK TOO, which the wake hook's header once
            // forbade outright. The prohibition was real but it was aimed at the
            // wrong thing: what must never happen is a blank with nobody left to
            // LIFT it, and the lift lives in `ApplyHideOnNavigation`, which now
            // runs whenever the switch is on rather than only in one mode. With
            // the lift following the switch, the switch is free to compose.
            camerawake::SetPollCallback (&ArchVizPanel::PollCameraOnce);
            camerawake::SetBlankOnInput (g_hideOnNav);
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
            //
            // ⚠️ THE GPU-STATE HOOKS GO ON AFTER THE PRESENT HOOK AND BEFORE THE
            // TIMER. They need the Present detour to exist -- it is what closes
            // a frame and what identifies Archicad's chain and context -- and
            // the timer is what feeds them the ACAPI reference to score
            // against, so a failure between the two must unwind both rather
            // than leave a mode that is recording with nothing to compare to.
            armed = dxgi::InstallPresentHook (error);
            if (armed && g_gpuState) {
                dxgi::renderstate::Reset ();
                dxgi::viewmatrix::Reset ();
                // ⚠️ WANTED HERE, INSTALLED LATER, AND THE ARM DOES NOT WAIT FOR
                // IT. The context hook reads its vtable off ARCHICAD'S OWN
                // immediate context, and that is only reachable through the swap
                // chain the present detour identifies -- which takes about sixty
                // of Archicad's frames. Installing inside the arm would mean
                // installing before the thing being hooked is known, which is
                // what the 2026-09-13 run did with a throwaway's vtable and why
                // it recorded nothing. The camera tick installs on the first tick
                // where the context is known; until then the mode is armed and
                // the hook is honestly reported as not up.
                dxgi::SetContextHookWanted (true);
            }
            armed = armed && ArchVizPanel::StartCameraSync (intervalMs);
            if (!armed) {
                dxgi::SetContextHookWanted (false);
                dxgi::RemoveContextHook ();
                dxgi::RemovePresentHook ();
            }
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
            //
            // ⚠️ IT SAMPLES AND PREDICTS EXACTLY AS `wakepredict` DOES, and until
            // 2026-08-28 it did neither (PLAT-RE116). It armed the bare WM_TIMER
            // and `ApplyPrediction` did not list it, so the one run that judged
            // blit-time reprojection composited perfectly and reprojected
            // faithfully -- onto a pose one starved timer behind. Reprojection can
            // only ever be as fresh as the newest pose it is handed, so measuring
            // it on the worst sample stream in the tree measured nothing. The
            // three mechanisms are independent and this mode is the one that needs
            // all of them; there is no configuration in which compositing is
            // wanted and fresher sampling is not.
            //
            // The callback goes on before the hook, for the reason `wake` gives:
            // the hook can fire on the very next message and RequestPoll does
            // nothing without one.
            dxgi::SetMarkerEnabled (true);
            dxgi::SetHostCompositeEnabled (true);
            camerawake::SetPollCallback (&ArchVizPanel::PollCameraOnce);
            camerawake::SetBlankOnInput (g_hideOnNav);
            armed = camerawake::Install (error) && dxgi::InstallPresentHook (error) &&
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
                camerawake::Remove ();
            }
            break;
        default:
            // Unreachable: NotYetBuilt already refused every other mode above.
            break;
    }

    if (!armed) {
        if (IsExperimental (mode))
            experimentguard::Disarm ();
        error = "arming camera sync mode '" + std::string (CameraSyncModeName (mode)) + "' failed; sync is now off";
        ArchVizLog ("camera sync mode: " + error);
        return false;
    }

    g_mode = mode;
    g_intervalMs = intervalMs;
    ArchVizLog ("camera sync mode: " + std::string (CameraSyncModeName (mode)) + " at " + std::to_string (intervalMs) +
                " ms, hideOnNav " + (g_hideOnNav ? "on" : "off") + ", gpuState " + (g_gpuState ? "on" : "off"));
    return true;
}

bool CurrentGpuState ()
{
    return g_gpuState;
}

CameraSyncMode CurrentCameraSyncMode ()
{
    return g_mode;
}

double CurrentPredictionScale ()
{
    return g_predictionScale;
}

bool CurrentHideOnNav ()
{
    return g_hideOnNav;
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

} // namespace archviz
} // namespace geomsrv
