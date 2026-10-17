// Bound by OVERLAY-INVARIANTS.md §§5,8,12: no renderer/camera modifications.
#include "SurfaceTakeoffOverlay.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayHudEvents.hpp"
#include "Geometry/MeshStore.hpp"
#include <windows.h>
#include <atomic>
#include <memory>

namespace geomsrv::archviz::surfacetakeoff {
namespace {
UINT_PTR s_timer = 0;
uint64_t s_snapshotId = 0, s_captureStamp = 0;
std::atomic<uint64_t> s_request { 0 };
std::shared_ptr<const Presentation> s_presentation;
UiState s_ui;
uint64_t s_eventSeq = 0;
bool Current (uint64_t id, uint64_t stamp)
{
    const auto snapshot = MeshStore::Get ().Current ();
    return snapshot && snapshot->id == id && snapshot->captureStamp == stamp &&
           MeshStore::Get ().CaptureStamp () == stamp;
}
void CALLBACK Tick (HWND, UINT, UINT_PTR timer, DWORD)
{
    if (!s_timer || timer != s_timer)
        return;
    if (Current (s_snapshotId, s_captureStamp)) {
        if (!s_presentation)
            return;
        const auto tail = overlayhudevents::Since (s_eventSeq, overlayhudevents::kCapacity);
        s_eventSeq = tail.lastSeq;
        bool changed = false;
        if (tail.gap) {
            s_ui.pending = s_ui.selected;
            s_ui.draftRules = s_ui.rules;
            s_ui.token = std::to_string (s_snapshotId) + ":" + std::to_string (s_eventSeq);
            s_ui.warning = "Material input expired. The last calculated list is retained; choose materials again.";
            changed = true;
        }
        else
            for (const auto& event : tail.events) {
                if (event.view != "3d" || event.layer != kLayer || event.panel < 0 || event.panel > 1 || !event.final)
                    continue;
                changed |= ApplyUiChange (*s_presentation, s_ui, event.kind, event.id, event.value, event.text);
            }
        if (changed) {
            overlaylayers::Set (UiLayer (*s_presentation, s_ui));
            overlaycontrol::PublishLayers ();
        }
        return;
    }
    // An old capture expiring must not revoke a newer worker request that has
    // already pinned the replacement capture. Project close/Clear do revoke it.
    KillTimer (nullptr, s_timer);
    s_timer = 0;
    s_snapshotId = s_captureStamp = 0;
    s_presentation.reset ();
    s_ui = {};
    overlaylayers::Clear (kLayer);
    overlaylayers::Layer stale;
    stale.name = kLayer;
    stale.views = overlaylayers::Views::ThreeD;
    overlaylayers::Panel panel;
    panel.title = "Exposed surfaces";
    overlaylayers::ApplyTheme (panel, overlaylayers::PanelTheme::Light);
    overlaylayers::PanelItem item;
    item.text = "Model/capture changed. Run Calculate again to refresh surface areas.";
    item.wrap = true;
    panel.items.push_back (item);
    stale.panels.push_back (panel);
    overlaylayers::Set (std::move (stale));
    overlaycontrol::PublishLayers ();
}
} // namespace
uint64_t BeginRequest ()
{
    return ++s_request;
}
bool Publish (const Presentation& presentation, uint64_t id, uint64_t stamp, uint64_t request, std::string& error)
{
    if (request != s_request.load ()) {
        error = "Surface request superseded before publication.";
        return false;
    }
    if (!Current (id, stamp)) {
        error = "Capture changed before surface overlay publication.";
        return false;
    }
    const auto overlay = overlaycontrol::Describe (overlaycontrol::Overlay::ThreeD);
    if (!overlay.wanted || !overlay.running) {
        error = "Enable the 3D overlay in a perspective 3D view first.";
        return false;
    }
    if (!s_timer)
        s_timer = SetTimer (nullptr, 0, 100, Tick);
    if (!s_timer) {
        error = "Could not watch surface-result freshness.";
        return false;
    }
    s_snapshotId = id;
    s_captureStamp = stamp;
    s_presentation = std::make_shared<const Presentation> (presentation);
    s_ui = InitialUi (presentation, std::to_string (request));
    s_eventSeq = overlayhudevents::LastSeq ();
    overlaylayers::Set (UiLayer (presentation, s_ui));
    overlaycontrol::PublishLayers ();
    return true;
}
void OnProjectClosed ()
{
    ++s_request;
    if (s_timer)
        KillTimer (nullptr, s_timer);
    s_timer = 0;
    s_snapshotId = s_captureStamp = 0;
    s_presentation.reset ();
    s_ui = {};
    s_eventSeq = 0;
    overlaylayers::Clear (kLayer);
}
void Clear ()
{
    OnProjectClosed ();
    overlaycontrol::PublishLayers ();
}
} // namespace geomsrv::archviz::surfacetakeoff
