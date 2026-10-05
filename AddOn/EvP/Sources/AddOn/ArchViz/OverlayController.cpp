// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/OverlayController -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/OverlayController.hpp"

#include "ResourceIds.hpp" // the two menu items whose checks mirror the intents

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/LayerOverlay3D.hpp"
#include "ArchViz/Dxgi/PlanGuest.hpp"
#include "ArchViz/Dxgi/SceneGuest.hpp"
#include "ArchViz/Dxgi/MarkerLadder.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/OverlayAnnotations.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHover3D.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudModel.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayRelease.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/OverlayVisibility.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp" // the Debug tab's console: what the user checks when something fails
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/PlanOverlayRuntime.hpp"
#include "ArchViz/SurfaceSwitch.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlaycontrol {

namespace {

namespace runtime = overlayruntime;

// ⚠️ WHETHER THE USER WANTS EACH OVERLAY, KEPT APART FROM WHICH RENDERER IS
// SERVING IT. The menu item and the verb set the intent; the front window decides
// whether its renderer runs now. Without that split, walking 3D -> plan -> 3D
// needs three clicks and reads as the overlay failing twice.
struct Intent {
    bool wanted = false;
    std::string code = "None";
    std::string message = "off";
    bool retryable = false;
};
Intent g_intent[2]; // indexed by Overlay
ViewKind g_servingView = ViewKind::Unknown;

Intent& IntentOf (Overlay which)
{
    return g_intent[which == Overlay::ThreeD ? 0 : 1];
}

bool AnyWanted ()
{
    return g_intent[0].wanted || g_intent[1].wanted;
}

ViewKind ViewOf (Overlay which)
{
    return which == Overlay::ThreeD ? ViewKind::ThreeD : ViewKind::FloorPlan;
}

void Narrate (const char* channel, const std::string& detail)
{
    char line[256] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE, "%-12s %s", channel, detail.c_str ());
    ArchVizLog (line);
}

// ⚠️ THE VIEWPORT COUNTS, NOT ONLY ITS WINDOW. The window can go without it --
// the tracker closed it when the floor plan (and the project) closed -- and a
// session judged by its window alone was invisible to every teardown after that:
// the viewport ran on, re-armed camera sync on the next project's plan, and the
// plan overlay was refused as "already running" (2026-09-27 22:13-22:15).
bool PortableRunning ()
{
    const DiligentViewport& viewport = DiligentViewport::Get ();
    return viewportoverlay::Current () != nullptr ||
           (viewport.IsRunning () && viewport.Mode () == SurfaceMode::Overlay);
}

// ⚠️ THE CONTROLLER NEEDS ITS OWN HEARTBEAT AND CANNOT BORROW THE
// RUNTIME'S. The injected runtime's timer only runs while the INJECTED runtime
// runs; a user serving a floor plan has no injected runtime at all, so the one
// case that most needs a view watcher is the one with nothing ticking. Half a
// second: this reads one ACAPI window id and compares an enum.
UINT_PTR g_timer = 0;
constexpr UINT kTickMs = 500;

// Whether the 3D overlay composes with a camera: the HUD is drawn with it at every Present.
bool ComposingWithCamera ()
{
    return runtime::Running () && runtime::Visible () &&
           dxgi::injection::GetArmState () == dxgi::injection::ArmState::Active && dxgi::injection::SnapshotValid ();
}

// Whether the 3D HUD is on screen: composed with the camera, or drawn alone before one was
// chosen in the last second (Dxgi/PrelockHud.hpp). Called from the HUD's message hook, on this
// thread: plain reads and atomics.
bool HudShown3D ()
{
    return ComposingWithCamera () ||
           (runtime::Running () && runtime::Visible () && dxgi::sceneguest::HudOnlyRecently (1000));
}

// ---- the 3D HUD ------------------------------------------------------------------
// The scene's legends where they were last laid out, the scale, and what the HUD last
// put on screen: a layout that puts the same is not uploaded or redrawn.
std::vector<overlayinput::Region> g_legends3D;
float g_scale3D = 1.0f;
uint64_t g_hudPrint3D = 0;

// The HUD handed to the guest, and where it now is to the input; true when what it
// draws changed.
bool PublishHud3D (overlayscene::Scene hud)
{
    overlayinput::HitMap map;
    map.dpiScale = g_scale3D;
    map.hand = hud.hand;
    map.regions = g_legends3D; // legends first: the panels are drawn over them
    map.regions.insert (map.regions.end (), hud.regions.begin (), hud.regions.end ());
    overlayinput::SetHitMap (overlayinput::View::ThreeD, std::move (map));
    const uint64_t print = overlayscene::Fingerprint (hud);
    if (print == g_hudPrint3D)
        return false;
    g_hudPrint3D = print;
    dxgi::sceneguest::PublishHud (std::move (hud), g_scale3D);
    return true;
}

// The 3D overlay's publications given back: what it drew, its HUD's engine, and where
// both were (OverlayRelease.hpp).
void Forget3D ()
{
    overlayrelease::ThreeD ();
    g_legends3D.clear ();
    g_hudPrint3D = 0;
}

// The input layer's refresh: the HUD laid out again for the pointer.
bool RefreshHud3D ()
{
    if (!runtime::Running ())
        return false;
    const std::vector<std::shared_ptr<const overlaylayers::Layer>> layers = overlaylayers::Layers ();
    overlayhud::Input input = overlayinput::TakeInput (overlayinput::View::ThreeD);
    overlayhover3d::Fill (input); // hover mode: what the pointer is on (D19)
    // ⚠️ THE HUD IS THERE WITH OR WITHOUT A LAYER: the overlay runs, its own pages say what.
    overlayscene::Scene hud = overlayscene::PrepareSceneHud (
        layers, overlayhudmodel::Prepare (overlayinput::View::ThreeD), g_scale3D, input, &g_legends3D);
    const bool changed = PublishHud3D (std::move (hud));
    // What the user did there may be the dock's circle or a layer hidden.
    FollowHudState ();
    return changed;
}

// The input layer's paced redraw: a still 3D view presents nothing by itself.
void RedrawHud3D ()
{
    if (runtime::Running () && CurrentView () == ViewKind::ThreeD)
        ACAPI_View_Redraw ();
}

// ⚠️ THE OWN PAGES MOVE WITHOUT THE POINTER -- the camera locks, the model is read, the
// selection changes -- so once a heartbeat the HUD is laid out again where the pointer is, no
// press replayed. A still view presents nothing, so a change asks for a frame -- except on
// Debug, whose figures move every time and whose redraw would be its own measurement: it is
// drawn with Archicad's next frame.
void HeartbeatHud3D ()
{
    if (!runtime::Running () || !overlayhud::HudOpen (*guesttext::HudState ()))
        return;
    overlayhud::Input input = overlayinput::CurrentInput (overlayinput::View::ThreeD);
    input.buttons.clear ();
    overlayhover3d::Fill (input);
    const bool changed = PublishHud3D (
        overlayscene::PrepareSceneHud (overlaylayers::Layers (), overlayhudmodel::Prepare (overlayinput::View::ThreeD),
                                       g_scale3D, input, &g_legends3D));
    // ⚠️ AND ONLY ONCE THE CAMERA IS LOCKED: before, a redraw is the cold start's budget's to
    // spend (OverlayRedrawBudget.hpp), never the HUD's -- the HUD drawn alone shows with the
    // frames Archicad presents anyway (Dxgi/PrelockHud.hpp).
    if (changed && ComposingWithCamera () && overlayhud::SelectedKey (*guesttext::HudState ()) != hudshell::kDebugKey)
        RedrawHud3D ();
}

// ⚠️ THE 3D HUD'S INPUT FOLLOWS THE CANVAS THE OVERLAY COMPOSES INTO -- the nominated
// swap chain's window, known once Archicad has presented through it, and a new one when
// the 3D window is closed and reopened. On this heartbeat rather than the runtime's
// tick; without a running 3D overlay it takes nothing (§8).
std::string g_inputError;
// The 3D guest drew less than it was given at the last publish (PublishLayers): said to the HUD's
// console when it starts, not at every publish -- and forgotten with the session (§8).
bool g_notDrawing3D = false;
void FollowHudInput ()
{
    if (!runtime::Running ()) {
        overlayinput::Detach (overlayinput::View::ThreeD);
        g_inputError.clear ();
        g_notDrawing3D = false;
        return;
    }
    const uint64_t chain = dxgi::MarkerTarget ();
    if (chain == 0)
        return;
    dxgi::ChainInfo chains[8];
    const size_t count = dxgi::GetChainInventory (chains, 8);
    for (size_t i = 0; i < count; ++i) {
        if (chains[i].swapChain != chain || chains[i].window == 0)
            continue;
        std::string error;
        overlayinput::HudOwner owner;
        owner.shown = &HudShown3D;
        owner.refresh = &RefreshHud3D;
        owner.redraw = &RedrawHud3D;
        owner.hovering = &overlayhover3d::Hovering;
        if (!overlayinput::Attach (overlayinput::View::ThreeD, HWND (uintptr_t (chains[i].window)), owner, error) &&
            error != g_inputError) {
            Narrate ("OVERLAY", "the 3D HUD takes no input: " + error);
            hudconsole::Warning ("Overlay", "the 3D HUD takes no input: " + error);
        }
        g_inputError = error;
        return;
    }
}

void CALLBACK TickProc (HWND, UINT, UINT_PTR, DWORD)
{
    overlaycontrol::FollowView ();
    FollowHudInput ();
    HeartbeatHud3D ();
}

void StartHeartbeat ()
{
    if (g_timer == 0)
        g_timer = ::SetTimer (nullptr, 0, kTickMs, TickProc);
}

void StopHeartbeat ()
{
    if (g_timer != 0) {
        ::KillTimer (nullptr, g_timer);
        g_timer = 0;
    }
}

// The heartbeat runs while either overlay is wanted and not a moment longer.
void SyncHeartbeat ()
{
    if (AnyWanted ())
        StartHeartbeat ();
    else
        StopHeartbeat ();
}

// Each menu item carries a check while its overlay is wanted, so the menu says what
// the user asked for without anyone reading a log. The other flags are kept.
void SyncMenuCheck (short menuResId, short itemIndex, bool checked)
{
    API_MenuItemRef item = {};
    item.menuResID = menuResId;
    item.itemIndex = itemIndex;
    GSFlags flags = 0;
    if (ACAPI_MenuItem_GetMenuItemFlags (&item, &flags) != NoError)
        return;
    GSFlags wanted = checked ? (flags | API_MenuItemChecked) : (flags & ~GSFlags (API_MenuItemChecked));
    if (wanted != flags)
        ACAPI_MenuItem_SetMenuItemFlags (&item, &wanted);
}

void SyncMenuChecks ()
{
    SyncMenuCheck (ArchVizOverlayMenuResId, ArchVizOverlayMenuItemIndex, g_intent[0].wanted);
    SyncMenuCheck (Overlay2DMenuResId, Overlay2DMenuItemIndex, g_intent[1].wanted);
}

// ⚠️ RENDERERS ONLY. `StopAll` clears the user's INTENT as well, and
// `FollowView` must not -- it stops one renderer in order to start another, and
// clearing intent mid-move would leave the overlay off with nobody having asked
// for that.
//
// ⚠️ `teardown` IS A PROJECT EVENT OR THE UNLOAD: the plan session then gives
// everything back without calling ACAPI, which is no longer the plan's to redraw.
// Otherwise the view changed, and the 3D session is suspended: its hooks go, what it
// learned about the camera is kept for the 3D window's return (OverlayCameraKeep).
void StopRenderers (bool teardown)
{
    if (teardown)
        runtime::Stop ();
    else
        runtime::Suspend ();
    overlayinput::Detach (overlayinput::View::ThreeD);
    if (planruntime::Running ()) {
        if (teardown)
            planruntime::Shutdown ();
        else
            planruntime::Stop ("the view changed");
    }
    if (PortableRunning ())
        ArchVizPanel::CloseDiligentOverlay ();
}

// MAIN THREAD, with `which`'s view in front: start its renderer and record the
// outcome on its intent.
//
// ⚠️ NO SILENT FALLBACK. A refusal is reported with its real code and nothing
// else is started in its place: quietly opening a different renderer would leave
// the user looking at an overlay they did not choose, unable to tell which one
// failed -- which is exactly how run sixty hid a wrong `BuildNotPinned` verdict
// behind a working portable overlay.
//
// ⚠️ AND NEVER TWO OVERLAYS ON ONE WINDOW, OR TWO OWNERS OF ONE HOOK. The portable
// window may still be up from an earlier session, and the other overlay's session
// shares the Present hook -- the first to stop would take it from both. Neither
// should be running with this view in front; both are stopped if they are.
void StartRenderer (Overlay which)
{
    Intent& intent = IntentOf (which);
    if (PortableRunning ())
        ArchVizPanel::CloseDiligentOverlay ();
    if (which == Overlay::ThreeD) {
        if (planruntime::Running ())
            planruntime::Stop ("the 3D overlay starts");
        if (runtime::Running ()) {
            intent.code = "None";
            intent.message = "running";
            return;
        }
        const runtime::StartResult started = runtime::Start ();
        intent.code = runtime::StartErrorName (started.code);
        intent.message = started.ok ? std::string ("running") : started.message;
        intent.retryable = !started.ok && started.retryable;
        if (started.ok) {
            Narrate ("BUILD", "supported");
            return;
        }
        Narrate ("OVERLAY", std::string ("3D NOT STARTED (") + intent.code + ") - " + intent.message);
        hudconsole::Error ("Overlay",
                           std::string ("the 3D overlay did not start (") + intent.code + "): " + intent.message);
        if (!started.retryable)
            Narrate ("OVERLAY", "this will not become true by waiting; the 3D overlay is unavailable here");
        return;
    }

    // ⚠️ THE 2D PATH TOUCHES NO 3D STATE AT ALL: no census, no host extraction, no
    // camera recognition, no camera anchor, no depth. It composes at the plan's own
    // Present, on the plan canvas's own chain, with a transform ACAPI gives it (§12).
    if (runtime::Running ())
        runtime::Stop ();
    if (planruntime::Running ()) {
        intent.code = "None";
        intent.message = "running";
        return;
    }
    Narrate ("2D RUNTIME", "starting the 2D overlay at the plan's Present");
    const planruntime::StartResult started = planruntime::Start ();
    intent.code = planruntime::StartErrorName (started.code);
    intent.message = started.ok ? std::string ("running") : started.message;
    // A frame record or a canvas not there yet may clear. The crash-loop guard holds
    // for the session, a Present slot another tool took holds until Archicad
    // restarts, and a missing content reader or a running session never clear.
    const planruntime::StartError code = started.code;
    intent.retryable = !started.ok && code != planruntime::StartError::NoContentReader &&
                       code != planruntime::StartError::AlreadyRunning && code != planruntime::StartError::Blocked &&
                       code != planruntime::StartError::PresentHook;
    if (!started.ok) {
        Narrate ("OVERLAY", std::string ("2D NOT STARTED (") + intent.code + ") - " + intent.message);
        hudconsole::Error ("Overlay", std::string ("the floor plan's overlay did not start (") + intent.code +
                                          "): " + intent.message);
    }
}

ViewKind KindOf (API_WindowTypeID type)
{
    if (type == APIWind_3DModelID)
        return ViewKind::ThreeD;
    if (type == APIWind_FloorPlanID)
        return ViewKind::FloorPlan;
    return ViewKind::Other;
}

// The window's own name for the log; the KIND decides what runs.
const char* WindowTypeName (API_WindowTypeID type)
{
    switch (type) {
        case APIWind_FloorPlanID:
            return "Floor Plan";
        case APIWind_SectionID:
            return "Section";
        case APIWind_DetailID:
            return "Detail";
        case APIWind_3DModelID:
            return "3D";
        case APIWind_LayoutID:
            return "Layout";
        case APIWind_DrawingID:
            return "Drawing";
        case APIWind_MasterLayoutID:
            return "Master Layout";
        case APIWind_ElevationID:
            return "Elevation";
        case APIWind_InteriorElevationID:
            return "Interior Elevation";
        case APIWind_WorksheetID:
            return "Worksheet";
        case APIWind_DocumentFrom3DID:
            return "3D Document";
        case APIWind_IESCommonDrawingID:
            return "Interactive Schedule";
        default:
            return "other window";
    }
}

// One pass of `Follow`: the window in front against the one being served.
void FollowOnce (const char* how)
{
    API_WindowInfo info = {};
    if (ACAPI_Window_GetCurrentWindow (&info) != NoError)
        return; // a transient read -- tear nothing down over it
    const ViewKind view = KindOf (info.typeID);
    if (view == g_servingView)
        return;

    // ⚠️ MEASURED, BECAUSE "SAFE" IS A CLAIM ABOUT TIME. While the 3D session runs
    // after its window has gone, its hooks sit in the frames of whatever is in front.
    const bool injected = runtime::Running ();
    const bool plan = planruntime::Running ();
    const bool portable = PortableRunning ();
    const ULONGLONG began = ::GetTickCount64 ();
    StopRenderers (false);
    const ULONGLONG took = ::GetTickCount64 () - began;
    char line[240] = {};
    if (injected || plan || portable)
        _snprintf_s (line, sizeof (line), _TRUNCATE, "%s -> %s (%s): %s in %llu ms", ViewKindName (g_servingView),
                     WindowTypeName (info.typeID), how,
                     injected ? "3D overlay suspended, hooks released,"
                     : plan   ? "plan overlay stopped, its hook released,"
                              : "portable plan overlay closed",
                     (unsigned long long) took);
    else
        _snprintf_s (line, sizeof (line), _TRUNCATE, "%s -> %s (%s): nothing was running", ViewKindName (g_servingView),
                     WindowTypeName (info.typeID), how);
    Narrate ("VIEW", line);

    g_servingView = view;
    if (view == ViewKind::ThreeD && IntentOf (Overlay::ThreeD).wanted)
        StartRenderer (Overlay::ThreeD);
    else if (view == ViewKind::FloorPlan && IntentOf (Overlay::TwoD).wanted)
        StartRenderer (Overlay::TwoD);
    else if (view == ViewKind::Other)
        Narrate ("OVERLAY", std::string ("no overlay is defined for the ") + WindowTypeName (info.typeID) +
                                "; the 3D overlay returns with the 3D window and the 2D overlay with the floor plan");
}

// ⚠️ NOT RE-ENTRANT, AND IT CAN BE ASKED TO BE: starting the 3D runtime can bring
// a window forward, and Archicad reports that change synchronously. A nested pass
// would stop the session the outer one is still starting, so it is deferred to
// after the outer pass instead.
bool g_following = false;
bool g_followAgain = false;

void Follow (const char* how)
{
    if (!AnyWanted ())
        return;
    if (g_following) {
        g_followAgain = true;
        return;
    }
    g_following = true;
    for (int pass = 0; pass < 3; ++pass) {
        g_followAgain = false;
        FollowOnce (how);
        if (!g_followAgain || !AnyWanted ())
            break;
    }
    g_following = false;
}

} // namespace

const char* ViewKindName (ViewKind kind)
{
    switch (kind) {
        case ViewKind::Unknown:
            return "Unknown";
        case ViewKind::ThreeD:
            return "3D";
        case ViewKind::FloorPlan:
            return "Floor Plan";
        case ViewKind::Other:
            return "other window";
    }
    return "Unknown";
}

ViewKind CurrentView ()
{
    API_WindowInfo info = {};
    if (ACAPI_Window_GetCurrentWindow (&info) != NoError)
        return ViewKind::Unknown;
    return KindOf (info.typeID);
}

bool InjectedOwnsView (ViewKind kind)
{
    // ⚠️ THE VIEW THE CALLER IS DRAWING DECIDES, NOT A GLOBAL. The injected
    // runtime being alive says something about the 3D window and nothing at all
    // about a plan; a suppression that read only the runtime's own flag switched
    // off the plan overlay's linework too.
    if (kind != ViewKind::ThreeD)
        return false;
    return runtime::Running () && runtime::Visible ();
}

const char* OverlayName (Overlay which)
{
    return which == Overlay::ThreeD ? "3D overlay" : "2D overlay";
}

// ⚠️ A FLOOR PLAN IS NOT 3D GEOMETRY AND MUST NOT BE BUILT FROM IT. The 2D
// overlay draws Archicad's own 2D representation -- a wall's plan outline is its
// CONNECTION polygon, trimmed where it meets other walls
// (`NativeCommands/PlanGeometryCommands.cpp`) -- never a storey cut of the 3D mesh,
// which is a section through the model with no 2D symbol, no reference line and
// no door or window break. And it is drawn at the plan's Present, not in a window
// of our own (finding 14): the portable window followed a poll the frame record
// measured a frame stale, 26-30 px at the median during a pan.
Outcome SetWanted (Overlay which, bool wanted, const char* how)
{
    Intent& intent = IntentOf (which);
    const ViewKind front = CurrentView ();
    Narrate ("OVERLAY", std::string (OverlayName (which)) + (wanted ? " on" : " off") + " (" + how + "), " +
                            ViewKindName (front) + " in front");
    hudconsole::Note ("Overlay", std::string (OverlayName (which)) + (wanted ? " on" : " off"));
    if (!wanted) {
        intent.wanted = false;
        if (which == Overlay::ThreeD) {
            const bool was = runtime::Running ();
            // Running or suspended behind another view: a kept camera goes with it.
            runtime::Stop ();
            overlayinput::Detach (overlayinput::View::ThreeD);
            // ⚠️ THE LAST FRAME STILL CARRIES THE OVERLAY, ITS HUD INCLUDED. A still 3D view
            // presents nothing by itself: without a redraw the panels stayed on screen,
            // unpressable, until the user navigated (the user, 2026-09-30: no way to get rid
            // of them). The plan's Stop redraws for the same reason. The hooks are out by now.
            if (was && front == ViewKind::ThreeD)
                ACAPI_View_Redraw ();
            // ⚠️ OFF IS OFF (the user, 2026-09-30: cleared completely, no resources held):
            // the model it occluded against and what it published go too (OverlayRelease.hpp).
            overlayrelease::Model ();
            Forget3D ();
        }
        else {
            if (planruntime::Running ())
                planruntime::Stop ("turned off");
            overlayrelease::Plan ();
        }
        // Both off: the layers and everything made for them.
        if (!AnyWanted ()) {
            overlayrelease::Shared ();
            FollowHudState ();
        }
        intent.code = "None";
        intent.message = "off";
        intent.retryable = false;
    }
    else {
        intent.wanted = true;
        // ⚠️ THE OVERLAY OR THE VIEWER, NEVER BOTH (the user, 2026-10-03; SurfaceSwitch.hpp).
        surfaceswitch::BeforeOverlayStarts ();
        // ⚠️ THE MENU AND THE HUD'S SWITCH OPEN THE HUD (the user, 2026-10-03: it always starts
        // with the overlay).
        if (std::string (how) == "menu" || std::string (how) == "hud")
            overlayhud::SetHudOpen (*guesttext::HudState (), true);
        // The Watch trace's annotations follow wherever an overlay is (OverlayAnnotations.hpp).
        overlayannotations::EnsureStarted ();
        // Laid out for 3D only while it is wanted (PublishLayers): now, before it draws.
        if (which == Overlay::ThreeD)
            PublishLayers ();
        g_servingView = front;
        if (front == ViewOf (which)) {
            StartRenderer (which);
        }
        else {
            intent.code = "Waiting";
            intent.message = std::string ("on; it starts when the ") + ViewKindName (ViewOf (which)) + " is in front";
            intent.retryable = false;
            Narrate ("OVERLAY", std::string (OverlayName (which)) + " " + intent.message);
        }
    }
    SyncHeartbeat ();
    SyncMenuChecks ();
    return Describe (which);
}

Outcome Toggle (Overlay which, const char* how)
{
    return SetWanted (which, !IntentOf (which).wanted, how);
}

Outcome Describe (Overlay which)
{
    const Intent& intent = IntentOf (which);
    Outcome outcome;
    outcome.wanted = intent.wanted;
    outcome.running = which == Overlay::ThreeD ? runtime::Running () : planruntime::Running ();
    outcome.code = intent.code;
    outcome.message = intent.message;
    outcome.retryable = intent.retryable;
    outcome.ok = intent.code == "None" || intent.code == "Waiting";
    return outcome;
}

// MAIN THREAD, from the runtime heartbeat. Keep the overlay on the window the
// user is actually looking at.
//
// ⚠️ THE SESSION FOR THE VIEW BEING LEFT IS TORN DOWN. An injected
// overlay holding Archicad own context vtable while the user works on a drawing
// earns nothing and risks everything; a portable overlay left over a 3D window
// is a second renderer on a window that already has one.
void FollowView ()
{
    Follow ("heartbeat");
}

void OnWindowChanged ()
{
    Follow ("notification");
}

void OnProjectClosed ()
{
    const bool active = AnyWanted () || runtime::Running () || planruntime::Running () || PortableRunning ();
    StopAll ();
    // The checks follow the intents off. `StopAll` cannot do this itself: it is also
    // the unload's teardown, where no ACAPI is called.
    SyncMenuChecks ();
    // ⚠️ THE CALLER'S LAYERS WERE THAT PROJECT'S COORDINATES (§8): drawn over the next
    // project they would be geometry from somewhere else, in the right place for nothing.
    // The storey slices, the Watch annotations and what the user did to that project's
    // panels -- a hidden overlay shown again with it -- were that project's too; and both
    // overlays are off, so they hold nothing (OverlayRelease.hpp). Not the model: the
    // extraction is asked to stop after this, and a project event must not wait on it.
    Forget3D ();
    overlayrelease::Plan ();
    overlayrelease::Shared ();
    overlayhudmodel::Forget ();
    FollowHudState ();
    if (active) {
        Narrate ("OVERLAY", "the project closed; both overlays are off -- start them again from the menu");
        hudconsole::Note ("Overlay", "the project closed: both overlays are off");
    }
}

void Mark (const std::string& note)
{
    Narrate ("MARK", note.empty () ? std::string ("(empty)") : note);
}

void SetEpochGate (bool enabled)
{
    dxgi::injection::freshness::SetEpochGate (enabled);
}

EpochGateCounts EpochGate ()
{
    const dxgi::injection::freshness::EpochGateReport report = dxgi::injection::freshness::GetEpochGate ();
    EpochGateCounts counts;
    counts.matched = report.matched;
    counts.mismatched = report.mismatched;
    counts.suppressed = report.suppressed;
    counts.behindMax = report.behindMax;
    counts.aheadMax = report.aheadMax;
    counts.enabled = report.enabled;
    return counts;
}

void SetMarkerLadder (bool enabled)
{
    dxgi::markerladder::SetEnabled (enabled);
    Narrate ("LADDER", enabled ? std::string ("armed -- A red, B yellow, C green, E magenta, "
                                              "top to bottom down the left edge")
                               : std::string ("off"));
}

LadderCounts MarkerLadderCounts ()
{
    const dxgi::markerladder::Stats stats = dxgi::markerladder::GetStats ();
    LadderCounts out;
    out.enabled = dxgi::markerladder::Enabled ();
    out.a = stats.painted[size_t (dxgi::markerladder::Rung::AfterModelDraw)];
    out.b = stats.painted[size_t (dxgi::markerladder::Rung::SceneBoundary)];
    out.c = stats.painted[size_t (dxgi::markerladder::Rung::NextTarget)];
    out.e = stats.painted[size_t (dxgi::markerladder::Rung::BeforePresent)];
    out.failures = stats.failures;
    return out;
}

GuestReport Guest ()
{
    GuestReport out;
    const dxgi::planguest::Stats plan = dxgi::planguest::GetStats ();
    out.plan.attached = plan.attached;
    out.plan.attachMilliseconds = plan.attachMilliseconds;
    out.plan.buildMilliseconds = plan.buildMilliseconds;
    out.plan.uploads = plan.uploads;
    out.plan.draws = plan.draws;
    out.plan.drawCalls = plan.drawCalls;
    out.plan.declinedNoTransform = plan.declinedNoTransform;
    out.plan.fills = plan.fills;
    out.plan.lines = plan.lines;
    out.plan.glyphVertices = plan.glyphVertices;
    out.plan.pages = plan.pages;
    out.plan.prepareMicroseconds = plan.prepareMicroseconds;
    out.plan.layersBuilt = plan.layersBuilt;
    out.plan.layersReused = plan.layersReused;
    out.plan.vertexBytes = plan.vertexBytes;
    out.plan.pageBytes = plan.pageBytes;
    out.plan.lastDrawMicroseconds = plan.lastDrawMicroseconds;
    out.plan.drawMicroseconds = plan.drawMicroseconds;
    out.plan.hudUploads = plan.hudUploads;
    out.plan.hudGlyphVertices = plan.hudGlyphVertices;
    out.plan.hudPrepareMicroseconds = plan.hudPrepareMicroseconds;
    out.plan.failure = plan.lastError;
    const dxgi::sceneguest::Stats scene = dxgi::sceneguest::GetStats ();
    out.scene.attached = scene.attached;
    out.scene.attachMilliseconds = scene.attachMilliseconds;
    out.scene.buildMilliseconds = scene.buildMilliseconds;
    out.scene.uploads = scene.uploads;
    out.scene.draws = scene.draws;
    out.scene.drawCalls = scene.drawCalls;
    out.scene.declinedNoCamera = scene.declinedNoCamera;
    out.scene.declinedNoViewport = scene.declinedNoViewport;
    out.scene.declinedFailed = scene.declinedFailed;
    out.scene.fills = scene.fills;
    out.scene.lines = scene.lines;
    out.scene.glyphVertices = scene.glyphVertices;
    out.scene.pages = scene.pages;
    out.scene.prepareMicroseconds = scene.prepareMicroseconds;
    out.scene.layersBuilt = scene.layersBuilt;
    out.scene.layersReused = scene.layersReused;
    out.scene.vertexBytes = scene.vertexBytes;
    out.scene.pageBytes = scene.pageBytes;
    out.scene.lastDrawMicroseconds = scene.lastDrawMicroseconds;
    out.scene.drawMicroseconds = scene.drawMicroseconds;
    out.scene.hudUploads = scene.hudUploads;
    out.scene.hudGlyphVertices = scene.hudGlyphVertices;
    out.scene.hudPrepareMicroseconds = scene.hudPrepareMicroseconds;
    out.scene.failure = scene.failure != nullptr ? scene.failure : "";
    return out;
}

InputCounts Input ()
{
    const overlayinput::Stats stats = overlayinput::GetStats ();
    InputCounts out;
    out.installed = stats.installed;
    out.attached3D = stats.attached[0];
    out.attachedPlan = stats.attached[1];
    out.regions3D = stats.regions[0];
    out.regionsPlan = stats.regions[1];
    out.seen = stats.seen;
    out.taken = stats.taken;
    out.takenPresses = stats.takenPresses;
    out.takenMoves = stats.takenMoves;
    out.passedOverHud = stats.passedOverHud;
    out.declinedHidden = stats.declinedHidden;
    out.refreshes = stats.refreshes;
    out.changes = stats.changes;
    out.redraws = stats.redraws;
    out.lastRedrawMicroseconds = stats.lastRedrawMicroseconds;
    out.maxRedrawMicroseconds = stats.maxRedrawMicroseconds;
    out.lastRefreshMicroseconds = stats.lastRefreshMicroseconds;
    out.maxRefreshMicroseconds = stats.maxRefreshMicroseconds;
    out.canvasOnThread3D = stats.canvasOnThread[0];
    out.canvasOnThreadPlan = stats.canvasOnThread[1];
    out.cursor3D = stats.subclassed[0];
    out.cursorPlan = stats.subclassed[1];
    out.cursorsSet = stats.cursorsSet;
    out.handsShown = stats.handsShown;
    out.mouseHook = stats.mouseHook;
    out.buttonsEaten = stats.buttonsEaten;
    out.buttonsLate = stats.buttonsLate;
    out.contextMenusSwallowed = stats.contextMenusSwallowed;
    return out;
}

void StopAll ()
{
    // Intent as well as renderers: this is the teardown entry point, and a timer
    // left armed in a DLL that is unloading is Windows calling into freed code.
    for (Intent& intent : g_intent)
        intent = Intent {};
    g_servingView = ViewKind::Unknown;
    StopHeartbeat ();
    StopRenderers (true);
    overlayinput::Shutdown ();
    overlayvisibility::SetContentShown (true);
    overlayvisibility::SetWireframeShown (true);
}

void PublishLayers ()
{
    // ⚠️ NOTHING LAID OUT FOR A 3D OVERLAY NOBODY WANTS (the user, 2026-09-30: off holds no
    // resources): no scene, no HUD engine. Turned on, it is laid out then (SetWanted); a
    // diagnostic that starts the renderer itself gets it while it runs.
    if (!IntentOf (Overlay::ThreeD).wanted && !runtime::Running ()) {
        Forget3D ();
        return;
    }
    // The content of the layers the user shows; the HUD of every one -- its Settings lists
    // the hidden ones too.
    const std::vector<std::shared_ptr<const overlaylayers::Layer>> all = overlaylayers::Layers ();
    const std::vector<std::shared_ptr<const overlaylayers::Layer>> layers = ShownLayers ();
    overlaylayers::Prepared3D prepared = overlaylayers::Prepare3D (layers);
    prepared.generation = overlaylayers::Generation ();
    dxgi::layers3d::Publish (std::move (prepared));

    // ⚠️ THE GUEST'S SHARE IS LAID OUT HERE, ON THE MAIN THREAD, BECAUSE THE TEXT
    // ENGINE IS: the render thread receives finished arrays and never shapes a glyph.
    bool text = false;
    for (const auto& layer : layers) {
        const bool drawn = overlaylayers::DrawnIn (layer->views, overlaylayers::Views::ThreeD);
        text = text || (drawn && (!layer->texts.empty () || !layer->dimensions.empty () || !layer->legends.empty ()));
    }
    const UINT dpi = ::GetDpiForSystem ();
    const float scale = dpi != 0 ? float (dpi) / 96.0f : 1.0f;
    overlayscene::Scene scene =
        overlayscene::PrepareScene (layers, text ? guesttext::Engine () : nullptr, &guesttext::EngineFor);
    scene.generation = overlaylayers::Generation ();
    g_legends3D = scene.regions;
    g_scale3D = scale;
    // The HUD panels are a stream of their own (OverlayScene.hpp PrepareSceneHud), laid
    // out for where the pointer is now.
    overlayscene::Scene hud =
        overlayscene::PrepareSceneHud (all, overlayhudmodel::Prepare (overlayinput::View::ThreeD), scale,
                                       overlayinput::CurrentInput (overlayinput::View::ThreeD), &g_legends3D);
    hud.generation = scene.generation;
    const overlayscene::Problems& problems = scene.problems;
    const uint32_t panelsNotDrawn = hud.problems.textsNotLaidOut;
    const bool notDrawing =
        problems.textsNotLaidOut + problems.dimensionsNotResolved + problems.truncated + panelsNotDrawn > 0;
    // To the console when it starts: a publish follows every layout, and its counts move.
    if (notDrawing && !g_notDrawing3D)
        hudconsole::Warning ("Overlay",
                             "not everything is drawn in 3D: " +
                                 (hud.problems.lastError.empty () ? problems.lastError : hud.problems.lastError));
    g_notDrawing3D = notDrawing;
    if (notDrawing) {
        Narrate ("OVERLAY", "3D guest NOT DRAWING " + std::to_string (problems.textsNotLaidOut) + " texts, " +
                                std::to_string (problems.dimensionsNotResolved) + " dimensions, " +
                                std::to_string (panelsNotDrawn) + " panels, " + std::to_string (problems.truncated) +
                                " past the budget: " +
                                (hud.problems.lastError.empty () ? problems.lastError : hud.problems.lastError));
    }
    dxgi::sceneguest::Publish (std::move (scene), scale);
    PublishHud3D (std::move (hud));
    if (runtime::Running () && CurrentView () == ViewKind::ThreeD)
        ACAPI_View_Redraw ();
}

Status GetStatus ()
{
    Status status;
    status.view = CurrentView ();
    status.want3D = IntentOf (Overlay::ThreeD).wanted;
    status.want2D = IntentOf (Overlay::TwoD).wanted;
    status.injectedRunning = runtime::Running ();
    status.planRunning = planruntime::Running ();
    status.portableRunning = PortableRunning ();
    return status;
}

} // namespace overlaycontrol
} // namespace archviz
} // namespace geomsrv
