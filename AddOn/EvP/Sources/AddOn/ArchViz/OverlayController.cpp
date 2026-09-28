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
#include "ArchViz/Dxgi/LayerOverlay3D.hpp"
#include "ArchViz/Dxgi/MarkerLadder.hpp"
#include "ArchViz/OverlayLayers.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/PlanOverlayRuntime.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#include <windows.h>

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

void CALLBACK TickProc (HWND, UINT, UINT_PTR, DWORD)
{
    overlaycontrol::FollowView ();
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
void StopRenderers (bool teardown)
{
    if (runtime::Running ())
        runtime::Stop ();
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
    if (!started.ok)
        Narrate ("OVERLAY", std::string ("2D NOT STARTED (") + intent.code + ") - " + intent.message);
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
                     injected ? "3D overlay stopped, hooks released,"
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
    if (!wanted) {
        intent.wanted = false;
        if (which == Overlay::ThreeD) {
            if (runtime::Running ())
                runtime::Stop ();
        }
        else if (planruntime::Running ()) {
            planruntime::Stop ("turned off");
        }
        intent.code = "None";
        intent.message = "off";
        intent.retryable = false;
    }
    else {
        intent.wanted = true;
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
    if (!overlaylayers::Layers ().empty ()) {
        overlaylayers::ClearAll ();
        PublishLayers ();
    }
    if (active)
        Narrate ("OVERLAY", "the project closed; both overlays are off -- start them again from the menu");
}

void Mark (const std::string& note)
{
    Narrate ("MARK", note.empty () ? std::string ("(empty)") : note);
}

void SetEpochGate (bool enabled)
{
    dxgi::injection::freshness::SetEpochGate (enabled);
}

bool EpochGateEnabled ()
{
    return dxgi::injection::freshness::EpochGate ();
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

bool MarkerLadderEnabled ()
{
    return dxgi::markerladder::Enabled ();
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

void StopAll ()
{
    // Intent as well as renderers: this is the teardown entry point, and a timer
    // left armed in a DLL that is unloading is Windows calling into freed code.
    for (Intent& intent : g_intent)
        intent = Intent {};
    g_servingView = ViewKind::Unknown;
    StopHeartbeat ();
    StopRenderers (true);
}

void PublishLayers ()
{
    overlaylayers::Prepared3D prepared = overlaylayers::Prepare3D (overlaylayers::Layers ());
    prepared.generation = overlaylayers::Generation ();
    dxgi::layers3d::Publish (std::move (prepared));
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
