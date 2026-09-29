// ArchViz/OverlayAnnotations -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/OverlayAnnotations.hpp"

#include "Annotation/RetainedTraceSelection.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OverlayAnnotationContent.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "Preview/PreviewRuntimeState.hpp" // a Watch trace is taken while the overlays want it

#include <windows.h>

#include <memory>

namespace geomsrv {
namespace archviz {
namespace overlayannotations {

namespace {

constexpr UINT kTickMs = 250;

bool g_enabled = false;
bool g_switchedOff = false; // a caller said off: EnsureStarted leaves it off
UINT_PTR g_timer = 0;
// What the layer shows: the trace's converted draw list and the selection in it.
std::shared_ptr<const annotation::DrawList> g_shownList;
size_t g_shownNode = SIZE_MAX;
size_t g_shownFrame = SIZE_MAX;
State g_state;

void Show (const std::optional<annotation::RetainedFrameSnapshot>& selected)
{
    if (!selected.has_value ()) {
        if (g_shownList != nullptr && overlaylayers::Clear (kLayerName))
            overlaycontrol::PublishLayers ();
        g_shownList = nullptr;
        g_state.haveFrame = false;
        g_state.primitives = g_state.drawn = 0;
        g_state.frame.clear ();
        return;
    }
    Built built = BuildLayer (selected->SelectedFrame ());
    g_shownList = selected->drawList;
    g_shownNode = selected->nodeIndex;
    g_shownFrame = selected->frameIndex;
    g_state.haveFrame = true;
    g_state.primitives = built.primitives;
    g_state.drawn = built.drawn;
    g_state.frame = "node " + std::to_string (selected->nodeIndex) + ", frame " + std::to_string (selected->frameIndex);
    const std::string refused = overlaylayers::Validate (built.layer);
    if (!refused.empty ()) {
        ArchVizLog ("OVERLAY ANNOTATIONS  NOT DRAWN (" + g_state.frame + "): " + refused);
        return;
    }
    overlaylayers::Set (std::move (built.layer));
    overlaycontrol::PublishLayers ();
    ArchVizLog ("OVERLAY ANNOTATIONS  " + g_state.frame + ": " + std::to_string (built.drawn) + " of " +
                std::to_string (built.primitives) + " primitives drawn");
}

void CALLBACK Tick (HWND, UINT, UINT_PTR, DWORD)
{
    if (!g_enabled)
        return;
    const std::optional<annotation::RetainedFrameSnapshot> selected = annotation::SelectedRetainedFrameSnapshotCopy ();
    const bool same = selected.has_value ()
                          ? (selected->drawList == g_shownList && selected->nodeIndex == g_shownNode &&
                             selected->frameIndex == g_shownFrame)
                          : g_shownList == nullptr;
    if (!same)
        Show (selected);
}

void Start ()
{
    g_enabled = true;
    g_state.enabled = true;
    // SetWatchTrace takes a trace for the overlays even with the palette's Preview off.
    evp::preview::PreviewRuntimeState::Get ().SetOverlayWatch (true);
    if (g_timer == 0)
        g_timer = ::SetTimer (nullptr, 0, kTickMs, Tick);
    // Whatever is selected now, at once.
    g_shownList = nullptr;
    g_shownNode = g_shownFrame = SIZE_MAX;
    Tick (nullptr, 0, 0, 0);
}

void Stop ()
{
    if (g_timer != 0) {
        ::KillTimer (nullptr, g_timer);
        g_timer = 0;
    }
    g_enabled = false;
    evp::preview::PreviewRuntimeState::Get ().SetOverlayWatch (false);
}

} // namespace

State Apply (bool enabled)
{
    g_switchedOff = !enabled;
    if (enabled) {
        if (!g_enabled)
            Start ();
    }
    else if (g_enabled) {
        Stop ();
        if (overlaylayers::Clear (kLayerName))
            overlaycontrol::PublishLayers ();
        g_shownList = nullptr;
        g_state = State {};
    }
    return g_state;
}

State Describe ()
{
    return g_state;
}

void EnsureStarted ()
{
    if (!g_switchedOff && !g_enabled)
        Start ();
}

void OnProjectClosed ()
{
    Stop ();
    g_shownList = nullptr;
    g_shownNode = g_shownFrame = SIZE_MAX;
    g_state = State {};
}

void Shutdown ()
{
    Stop ();
}

} // namespace overlayannotations
} // namespace archviz
} // namespace geomsrv
