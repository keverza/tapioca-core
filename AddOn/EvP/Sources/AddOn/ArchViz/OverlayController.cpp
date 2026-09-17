// ArchViz/OverlayController -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/OverlayController.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlaycontrol {

namespace {

namespace runtime = overlayruntime;

std::string g_lastCode = "None";
std::string g_lastMessage;

void Narrate (const char* channel, const std::string& detail)
{
    char line[256] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE, "%-12s %s", channel, detail.c_str ());
    ArchVizLog (line);
}

bool PortableRunning ()
{
    return viewportoverlay::Current () != nullptr;
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
    if (info.typeID == APIWind_3DModelID)
        return ViewKind::ThreeD;
    if (info.typeID == APIWind_FloorPlanID)
        return ViewKind::FloorPlan;
    return ViewKind::Other;
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
                g_lastCode = "None";
                g_lastMessage = "stopped by the menu";
                return;
            }
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
                Narrate ("OVERLAY", "plan overlay closed");
                return;
            }
            Narrate ("2D RUNTIME", "starting the plan overlay");
            ArchVizPanel::OpenDiligentOverlay ();
            g_lastCode = "None";
            g_lastMessage = "plan overlay requested";
            return;
        }

        case ViewKind::Other:
        case ViewKind::Unknown:
        default:
            // ⚠️ EXPLICIT, NOT A FALLBACK. Sections, elevations and layouts each
            // need their own camera measurement; guessing one of the two existing
            // renderers would draw a model over a drawing.
            g_lastCode = "UnsupportedView";
            g_lastMessage = std::string ("no overlay is defined for the ") + ViewKindName (view);
            Narrate ("OVERLAY", "NOT STARTED (UnsupportedView) - " + g_lastMessage);
            return;
    }
}

void StopAll ()
{
    if (runtime::Running ())
        runtime::Stop ();
    if (PortableRunning ())
        ArchVizPanel::CloseDiligentOverlay ();
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
