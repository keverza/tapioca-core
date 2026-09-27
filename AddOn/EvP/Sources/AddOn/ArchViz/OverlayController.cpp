// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/OverlayController -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/OverlayController.hpp"

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/Dxgi/MarkerLadder.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#include <windows.h>

#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlaycontrol {

void StartPlanOverlay (); // below; `Follow` starts it

namespace {

namespace runtime = overlayruntime;

std::string g_lastCode = "None";
std::string g_lastMessage;

// ⚠️ WHETHER THE USER WANTS AN OVERLAY AT ALL, KEPT APART FROM WHICH
// RENDERER IS SERVING IT. The menu toggles the intent; the front window decides
// which renderer answers. Without that split, walking 3D -> plan -> 3D needs
// three clicks and reads as the overlay failing twice.
bool g_requested = false;
ViewKind g_servingView = ViewKind::Unknown;

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

// ⚠️ RENDERERS ONLY. `StopAll` clears the user's INTENT as well, and
// `FollowView` must not -- it stops one renderer in order to start another, and
// clearing intent mid-move would leave the overlay off with nobody having asked
// for that.
void StopRenderers ()
{
    if (runtime::Running ())
        runtime::Stop ();
    if (PortableRunning ())
        ArchVizPanel::CloseDiligentOverlay ();
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
    const bool portable = PortableRunning ();
    const ULONGLONG began = ::GetTickCount64 ();
    StopRenderers ();
    const ULONGLONG took = ::GetTickCount64 () - began;
    char line[240] = {};
    if (injected || portable)
        _snprintf_s (line, sizeof (line), _TRUNCATE, "%s -> %s (%s): %s in %llu ms", ViewKindName (g_servingView),
                     WindowTypeName (info.typeID), how,
                     injected ? "3D overlay stopped, hooks released," : "plan overlay closed",
                     (unsigned long long) took);
    else
        _snprintf_s (line, sizeof (line), _TRUNCATE, "%s -> %s (%s): nothing was running", ViewKindName (g_servingView),
                     WindowTypeName (info.typeID), how);
    Narrate ("VIEW", line);

    g_servingView = view;
    if (view == ViewKind::ThreeD) {
        const runtime::StartResult started = runtime::Start ();
        g_lastCode = runtime::StartErrorName (started.code);
        g_lastMessage = started.message;
        if (!started.ok)
            Narrate ("OVERLAY", std::string ("NOT STARTED (") + g_lastCode + ") - " + g_lastMessage);
    }
    else if (view == ViewKind::FloorPlan) {
        StartPlanOverlay ();
    }
    else {
        Narrate ("OVERLAY", std::string ("no overlay is defined for the ") + WindowTypeName (info.typeID) +
                                "; it returns with the 3D window");
    }
}

// ⚠️ NOT RE-ENTRANT, AND IT CAN BE ASKED TO BE: starting the 3D runtime can bring
// a window forward, and Archicad reports that change synchronously. A nested pass
// would stop the session the outer one is still starting, so it is deferred to
// after the outer pass instead.
bool g_following = false;
bool g_followAgain = false;

void Follow (const char* how)
{
    if (!g_requested)
        return;
    if (g_following) {
        g_followAgain = true;
        return;
    }
    g_following = true;
    for (int pass = 0; pass < 3; ++pass) {
        g_followAgain = false;
        FollowOnce (how);
        if (!g_followAgain || !g_requested)
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

// MAIN THREAD. Start the plan overlay.
//
// ⚠️ A FLOOR PLAN IS NOT 3D GEOMETRY AND MUST NOT BE BUILT FROM IT.
// It is Archicad's 2D representation, reached through a different API entirely:
// the `ACAPI_DrawingPrimitive_*` family, which calls back with the drawing
// primitives an element actually contributes to the drawing.
// `NativeCommands/PlanGeometryCommands.cpp` already does exactly this --
// `GetWallPlanOutlines` and `GetPlanElementEdges` collect those primitives --
// and `DiligentViewport::SetPlanAnchors` is the drawing half.
//
// ⚠️ AN EARLIER VERSION OF THIS FUNCTION ASKED FOR STOREY SLICES
// INSTEAD. `StorySliceAccumulator` cuts the 3D MESH against storey planes and
// unions the loops, which is a SECTION THROUGH THE MODEL and not a plan: no 2D
// symbol, no wall reference line, no door or window break, and it agrees with
// the drawing only where the two happen to coincide. Wrong mechanism, removed.
void StartPlanOverlay ()
{
    Narrate ("2D RUNTIME", "starting the plan overlay");
    ArchVizPanel::OpenDiligentOverlay ();
    g_lastCode = "None";
    g_lastMessage = "plan overlay requested; 2D outlines are not wired yet";
    // ⚠️ AND IT SAYS SO. The window opens and follows the plan
    // camera, which is the part that works; the wall outlines are not fed to it
    // yet. Claiming otherwise in a log is how a half-finished path gets believed.
    Narrate ("2D RUNTIME", "NOTE: wall outlines not yet fed from GetWallPlanOutlines");
}

void Toggle ()
{
    const ViewKind view = CurrentView ();
    Narrate ("OVERLAY MENU", std::string ("requested for ") + ViewKindName (view));

    switch (view) {
        case ViewKind::ThreeD: {
            // ⚠️ THE PORTABLE OVERLAY IS NOT THE 3D RENDERER ANY MORE, but it may
            // still be up from a plan session. Leaving it running over the 3D
            // window would put two overlays on one window, which is the state the
            // suppression hack existed to paper over.
            if (PortableRunning ())
                ArchVizPanel::CloseDiligentOverlay ();

            if (runtime::Running ()) {
                runtime::Stop ();
                g_requested = false;
                g_servingView = ViewKind::Unknown;
                StopHeartbeat ();
                g_lastCode = "None";
                g_lastMessage = "stopped by the menu";
                return;
            }
            g_requested = true;
            g_servingView = view;
            StartHeartbeat ();
            const runtime::StartResult started = runtime::Start ();
            g_lastCode = runtime::StartErrorName (started.code);
            g_lastMessage = started.message;
            if (started.ok) {
                Narrate ("BUILD", "supported");
                return;
            }
            // ⚠️ NO SILENT FALLBACK. The refusal is reported with its real code
            // and the menu stops. Quietly starting a different renderer would
            // leave the user looking at an overlay they did not choose, unable to
            // tell which one failed -- which is exactly how run sixty hid a wrong
            // `BuildNotPinned` verdict behind a working portable overlay.
            Narrate ("OVERLAY", std::string ("NOT STARTED (") + g_lastCode + ") - " + g_lastMessage);
            if (!started.retryable)
                Narrate ("OVERLAY", "this will not become true by waiting; the 3D overlay is unavailable here");
            return;
        }

        case ViewKind::FloorPlan: {
            // ⚠️ THE PLAN PATH TOUCHES NO 3D STATE AT ALL: no census, no host
            // extraction, no Present injection, no camera anchor, no depth. A 3D
            // session that happens to be running keeps running, in its own window.
            if (PortableRunning ()) {
                ArchVizPanel::CloseDiligentOverlay ();
                g_requested = false;
                g_servingView = ViewKind::Unknown;
                StopHeartbeat ();
                Narrate ("OVERLAY", "plan overlay closed");
                return;
            }
            g_requested = true;
            g_servingView = view;
            StartHeartbeat ();
            StartPlanOverlay ();
            return;
        }

        case ViewKind::Other:
        case ViewKind::Unknown:
        default:
            // ⚠️ WHAT WAS WANTED CAN ALWAYS BE UNWANTED. The 3D session stopped when
            // this view came forward, but the intent did not; a click here ends it,
            // or the overlay would come back on the next return to 3D.
            if (g_requested) {
                StopAll ();
                g_lastCode = "None";
                g_lastMessage = std::string ("turned off in the ") + ViewKindName (view);
                Narrate ("OVERLAY", "turned off; it will not return with the 3D window");
                return;
            }
            // ⚠️ EXPLICIT, NOT A FALLBACK. Sections, elevations and layouts each
            // need their own camera measurement; guessing one of the two existing
            // renderers would draw a model over a drawing.
            g_lastCode = "UnsupportedView";
            g_lastMessage = std::string ("no overlay is defined for the ") + ViewKindName (view);
            Narrate ("OVERLAY", "NOT STARTED (UnsupportedView) - " + g_lastMessage);
            return;
    }
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
    const bool active = g_requested || runtime::Running () || PortableRunning ();
    StopAll ();
    if (active)
        Narrate ("OVERLAY", "the project closed; the overlay is off -- start it again from the menu");
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
    g_requested = false;
    g_servingView = ViewKind::Unknown;
    StopHeartbeat ();
    StopRenderers ();
}

Status GetStatus ()
{
    Status status;
    status.view = CurrentView ();
    status.injectedRunning = runtime::Running ();
    status.portableRunning = PortableRunning ();
    status.lastCode = g_lastCode;
    status.lastMessage = g_lastMessage;
    return status;
}

} // namespace overlaycontrol
} // namespace archviz
} // namespace geomsrv
