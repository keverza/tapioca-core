// OVERLAY-INVARIANTS.md §8/§11/§15: reuse the scoped input owner and the editor's
// main-thread HUD. No renderer or session is accessed across its thread boundary.
#include "ArchViz/ViewerEditor.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/SurfaceSwitch.hpp"
#include <atomic>
#include <chrono>
#include <mutex>

namespace geomsrv::archviz::vieweredit {
namespace {
std::atomic<bool> s_active { false };
std::atomic<bool> s_running { false }, s_requested { false };
std::atomic<bool> s_analysisRequested { false };
std::atomic<uint64_t> s_generation { 0 };
std::atomic<int64_t> s_drawn { 0 };
std::mutex s_mutex;
Frame s_frame;
hudshell::DockPlacement s_dock;
uint32_t s_fontStep = hudshell::kFontStepDefault;
bool s_placementRequested = false;
std::unique_ptr<Drawing> s_pending;
HWND s_window = nullptr;
UINT_PTR s_timer = 0;
std::shared_ptr<const overlaylayers::Layer> s_layer;
uint64_t s_hudHash = 0;
bool s_refreshing = false;

int64_t Now ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now ().time_since_epoch ())
        .count ();
}
bool Shown ()
{
    return Active () && Now () - s_drawn.load () < 250;
}
void Redraw ()
{
} // the viewer presents continuously; never redraw Archicad
bool Hovering ()
{
    return Active ();
}

bool Refresh ()
{
    if (!Running () || s_refreshing)
        return false;
    if (!DiligentViewport::Get ().IsRunning ()) {
        Stop (true); // renderer failure must not silently discard the document
        return false;
    }
    const auto state = guesttext::HudState ();
    if (s_requested.exchange (false)) {
        {
            std::lock_guard<std::mutex> lock (s_mutex);
            if (s_placementRequested) {
                state->host.dock = s_dock;
                state->fontStep = s_fontStep;
                s_placementRequested = false;
            }
        }
        if (overlayhud::OpenVirtualEditorWorkflow (*state)) {
            overlayinput::HudOwner owner { &Shown, &Refresh, &Redraw, &Hovering };
            std::string error;
            if (overlayinput::Attach (overlayinput::View::ThreeD, s_window, owner, error)) {
                overlayhud::RestoreVirtualEditor (*state, overlayhud::TakeVirtualEditor (*state));
                s_active.store (true);
            }
            else
                ArchVizLog ("VIEWER EDITOR input could not start: " + error);
        }
    }
    if (!Active ())
        return false;
    if (!overlayhud::VirtualEditorActive (*state)) {
        s_active.store (false);
        overlayinput::Detach (overlayinput::View::ThreeD);
        overlayinput::EnableRoofEditKeys (false);
        return false;
    }
    Frame frame;
    {
        std::lock_guard<std::mutex> lock (s_mutex);
        frame = s_frame;
    }
    if (!frame.width || !frame.height)
        return false;
    const auto currentInput = overlayinput::CurrentInput (overlayinput::View::ThreeD);
    if (currentInput.width != float (frame.width) || currentInput.height != float (frame.height))
        return false; // resize: wait for the renderer's matching client size, never pick against stale pixels
    s_refreshing = true;
    auto input = overlayinput::TakeInput (overlayinput::View::ThreeD);
    input.width = float (frame.width);
    input.height = float (frame.height);
    input.project = [frame] (double x, double y, double z, float& px, float& py) {
        return frame.Project (x, y, z, px, py);
    };
    input.hasCameraDirection = true;
    // The inverse view's forward axis, in world coordinates (row-vector camera).
    input.cameraDirection[0] = -frame.view[2];
    input.cameraDirection[1] = -frame.view[6];
    input.cameraDirection[2] = -frame.view[10];
    auto* engine = guesttext::Hud (overlayinput::View::ThreeD);
    if (!engine) {
        s_refreshing = false;
        return false;
    }
    overlayhud::OwnPages own;
    own.standalone = true;
    own.viewerSurface = true;
    own.overlay.tip = "Switch back to the Archicad overlay; keep this editor";
    own.viewer.active = true;
    own.viewer.phase = hudshell::Phase::Ready;
    own.viewer.tip = "Editing in the Tapioca 3D viewer";
    engine->SetOwnPages (std::move (own));
    const float scale = float (::GetDpiForWindow (s_window)) / 96.0f;
    auto drawing = std::make_unique<Drawing> ();
    drawing->scale = scale;
    drawing->hud = overlayscene::PrepareSceneHud (overlaylayers::Layers (), engine, scale, input);
    if (engine->Selected () == "@viewer.analysis") {
        // Navigation releases input, not the document. Renderer-owned analysis keeps
        // its own state; no Session or ImGui pointer crosses the thread boundary.
        auto session = overlayhud::TakeVirtualEditor (*state);
        overlayhud::RestoreVirtualEditor (*state, std::move (session));
        overlayhud::SetEditLocked (*state, false);
        overlayinput::Detach (overlayinput::View::ThreeD);
        overlayinput::EnableRoofEditKeys (false);
        {
            std::lock_guard<std::mutex> lock (s_mutex);
            s_dock = state->host.dock;
            s_dock.workflowFlyoutOpen = false;
            s_fontStep = state->fontStep;
        }
        s_analysisRequested.store (true);
        s_active.store (false);
        s_refreshing = false;
        return true;
    }
    overlayinput::HitMap hit;
    hit.regions = drawing->hud.regions;
    hit.dpiScale = scale;
    hit.hand = drawing->hud.hand;
    hit.cursor = drawing->hud.cursor;
    hit.locked = drawing->hud.locked;
    overlayinput::SetHitMap (overlayinput::View::ThreeD, std::move (hit));
    // The same session publishes after its input was consumed; never re-seed it.
    overlaylayers::Layer layer;
    bool clear = false;
    if (overlayhud::FollowVirtualEditor (*state, layer, clear)) {
        if (clear)
            overlaylayers::Clear (editlayers::kLayer);
        else
            overlaylayers::Set (std::move (layer));
    }
    if (!overlayhud::VirtualEditorActive (*state)) {
        s_refreshing = false;
        s_active.store (false);
        overlayinput::Detach (overlayinput::View::ThreeD);
        overlayinput::EnableRoofEditKeys (false);
        return true;
    }
    std::vector<std::shared_ptr<const overlaylayers::Layer>> layers;
    for (const auto& candidate : overlaylayers::Layers ())
        if (candidate->name == editlayers::kLayer && overlayhud::ContentShown (*state) &&
            overlayhud::LayerShown (*state, candidate->name))
            layers.push_back (candidate);
    const auto current = layers.empty () ? nullptr : layers.front ();
    drawing->sceneChanged = current != s_layer;
    if (drawing->sceneChanged) {
        drawing->scene = overlayscene::PrepareScene (layers, guesttext::Engine ());
        s_layer = current;
    }
    const auto hash = overlayscene::Fingerprint (drawing->hud);
    if (hash != s_hudHash || drawing->sceneChanged) {
        s_hudHash = hash;
        std::lock_guard<std::mutex> lock (s_mutex);
        // Latest HUD wins, but a skipped render frame must not lose a scene update.
        if (s_pending && s_pending->sceneChanged && !drawing->sceneChanged) {
            drawing->scene = std::move (s_pending->scene);
            drawing->sceneChanged = true;
        }
        s_pending = std::move (drawing);
    }
    if (!overlayinput::EnableRoofEditKeys (overlayhud::EditLocked (*state) && overlayhud::ContentShown (*state))) {
        s_refreshing = false;
        ArchVizLog ("VIEWER EDITOR keyboard ownership refused; editor retained");
        Stop (true);
        return false;
    }
    if (overlayhud::TakeViewerRequest (*state))
        surfaceswitch::Request (surfaceswitch::Surface::Overlay);
    s_refreshing = false;
    return true;
}
void CALLBACK Tick (HWND, UINT, UINT_PTR, DWORD)
{
    Refresh ();
}
} // namespace

bool Active ()
{
    return s_active.load ();
}
bool Running ()
{
    return s_running.load ();
}
void RequestWorkflow (const hudshell::DockPlacement& dock, uint32_t fontStep)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_dock = dock;
    s_dock.workflowFlyoutOpen = false;
    s_fontStep = fontStep;
    s_placementRequested = true;
    s_requested.store (true);
}
bool TakeAnalysisRequest (hudshell::DockPlacement& dock, uint32_t& fontStep)
{
    if (!s_analysisRequested.exchange (false))
        return false;
    std::lock_guard<std::mutex> lock (s_mutex);
    dock = s_dock;
    fontStep = s_fontStep;
    return true;
}
uint64_t Generation ()
{
    return s_generation.load ();
}
void PublishFrame (Frame frame)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    if (Running ())
        s_frame = std::move (frame);
}
std::unique_ptr<Drawing> TakeDrawing ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return std::move (s_pending);
}
void Drawn (uint64_t generation)
{
    if (Active () && generation == Generation ())
        s_drawn.store (Now ());
}

bool Start (void* window)
{
    if (Running ())
        return true;
    s_window = static_cast<HWND> (window);
    s_generation.fetch_add (1);
    s_running.store (true);
    s_drawn.store (0);
    s_requested.store (overlayhud::VirtualEditorActive (*guesttext::HudState ()));
    if (!(s_timer = ::SetTimer (nullptr, 0, 15, Tick))) {
        ArchVizLog ("VIEWER EDITOR timer could not start");
        Stop (true);
        return false;
    }
    return true;
}

void Stop (bool preserve)
{
    if (!s_running.exchange (false))
        return;
    s_active.store (false);
    s_requested.store (false);
    s_analysisRequested.store (false);
    s_generation.fetch_add (1);
    if (s_timer)
        ::KillTimer (nullptr, s_timer);
    s_timer = 0;
    overlayinput::Detach (overlayinput::View::ThreeD);
    overlayinput::EnableRoofEditKeys (false);
    const auto state = guesttext::HudState ();
    if (preserve) {
        auto session = overlayhud::TakeVirtualEditor (*state);
        overlayhud::RestoreVirtualEditor (*state, std::move (session));
    }
    else {
        overlayhud::StopVirtualEditor (*state, "viewer closed");
        overlaylayers::Clear (editlayers::kLayer);
    }
    guesttext::ReleaseHud (overlayinput::View::ThreeD);
    s_window = nullptr;
    s_layer.reset ();
    s_hudHash = 0;
    std::lock_guard<std::mutex> lock (s_mutex);
    s_frame = {};
    s_dock = {};
    s_placementRequested = false;
    s_pending.reset ();
}
} // namespace geomsrv::archviz::vieweredit
