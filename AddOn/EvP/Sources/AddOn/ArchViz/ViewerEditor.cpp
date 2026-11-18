// OVERLAY-INVARIANTS.md §8/§11/§15: reuse the scoped input owner and the editor's
// main-thread HUD. No renderer or session is accessed across its thread boundary.
#include "ArchViz/ViewerEditor.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiagramModel.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudMassing.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/OverlayHudModel.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/VirtualEditorTerrainCapture.hpp"
#include "ArchViz/VirtualEditorMaterialPicker.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/SurfaceSwitch.hpp"
#include "ArchViz/SurfaceTakeoffLayer.hpp"
#include "ArchViz/ViewportCursor.hpp"
#include "Geometry/SurfaceTakeoffController.hpp"
#include "AddOnCommands.hpp"
#include "Python/MainThreadGate.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

namespace geomsrv::archviz::vieweredit {
namespace {
std::atomic<bool> s_active { false };
std::atomic<bool> s_running { false }, s_requested { false };
std::atomic<bool> s_analysisRequested { false };
std::atomic<bool> s_massingRequested { false };
bool s_massing = false; // MAIN THREAD: the host shows the Massing workflow (#201)
std::atomic<uint64_t> s_generation { 0 };
std::atomic<int64_t> s_drawn { 0 };
std::mutex s_mutex;
Frame s_frame;
hudshell::DockPlacement s_dock;
uint32_t s_fontStep = hudshell::kFontStepDefault;
bool s_placementRequested = false;
std::unique_ptr<Drawing> s_pending;
std::optional<FocusBox> s_focus; // #124: the latest box to frame; the render thread takes it
HWND s_window = nullptr;
UINT_PTR s_timer = 0;
std::shared_ptr<const overlaylayers::Layer> s_layer;
uint64_t s_hudHash = 0;
bool s_refreshing = false;
std::optional<surfacetakeoff::PageAction> s_surfaceAction;
std::string s_surfaceError;
std::atomic<std::shared_ptr<const SurfaceHighlight>> s_surfaceHighlight;
surfacetakeoff::Presentation s_surfacePresentation;
surfacetakeoff::UiState s_surfaceUi;
uint64_t s_preparedGeneration = 0;

void RefreshSurfaceHighlight (const std::optional<surfacetakeoff::PageAction>& action)
{
    const auto view = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ().TryPoll ();
    if (!view)
        return;
    if (view->state != geomsrv::surfacetakeoff::ControllerState::Current || !view->lastGood ||
        !view->lastGood->viewerScene || view->lastGood->generation != view->generation) {
        s_surfaceHighlight.store (nullptr);
        s_surfacePresentation = {};
        s_preparedGeneration = 0;
        return;
    }
    const auto& accepted = *view->lastGood;
    const auto old = s_surfaceHighlight.load ();
    const bool update = action && action->generation == accepted.generation && old &&
                        old->summary.generation == accepted.generation &&
                        surfacetakeoff::ApplyHighlightSelection (s_surfacePresentation, s_surfaceUi, *action);
    if (s_preparedGeneration == accepted.generation && !update)
        return;
    std::string error;
    const auto result = std::static_pointer_cast<const geomsrv::surfacetakeoff::Result> (accepted.payload);
    if (s_preparedGeneration != accepted.generation) {
        s_preparedGeneration = accepted.generation;
        s_surfaceHighlight.store (nullptr);
        s_surfacePresentation = {};
        if (!surfacetakeoff::BuildFromResult (*accepted.snapshot, *result, s_surfacePresentation, error)) {
            std::lock_guard<std::mutex> lock (s_mutex);
            s_surfaceError = std::move (error);
            return;
        }
        s_surfaceUi = surfacetakeoff::InitialUi (s_surfacePresentation, "");
    }
    auto highlight = std::make_shared<SurfaceHighlight> ();
    highlight->capture = accepted.snapshot;
    highlight->summary.generation = accepted.generation;
    auto layer = std::make_shared<const overlaylayers::Layer> (
        surfacetakeoff::HighlightLayer (s_surfacePresentation, s_surfaceUi, highlight->summary, error));
    highlight->scene = overlayscene::PrepareScene ({ layer }, nullptr);
    if (highlight->scene.fills.size () / 3 != highlight->summary.triangles) {
        std::lock_guard<std::mutex> lock (s_mutex);
        s_surfaceError = "Viewer highlight triangles differ from the validated result.";
        return;
    }
    if (MeshStore::Get ().IsCurrentCapture (highlight->capture, true))
        s_surfaceHighlight.store (std::move (highlight));
}

void ReportSurfaceError (uint64_t generation, const GS::UniString& error)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    if (Running () && Generation () == generation)
        s_surfaceError = error.ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

void FollowSurfaceTakeoff ()
{
    std::optional<surfacetakeoff::PageAction> action;
    {
        std::lock_guard<std::mutex> lock (s_mutex);
        action = std::exchange (s_surfaceAction, std::nullopt);
    }
    RefreshSurfaceHighlight (action);
    if (!action || (action->kind != surfacetakeoff::PageAction::Kind::Calculate &&
                    action->kind != surfacetakeoff::PageAction::Kind::Cancel))
        return;
    const auto generation = Generation ();
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().PostDeferred (
            [action = *action, generation] {
                if (!Running () || Generation () != generation)
                    return;
                auto& controller = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ();
                if (action.kind == surfacetakeoff::PageAction::Kind::Cancel) {
                    controller.Cancel (action.generation);
                    return;
                }
                const auto view = controller.TryPoll ();
                if (!view || view->generation != action.generation ||
                    view->state == geomsrv::surfacetakeoff::ControllerState::Pending) {
                    ReportSurfaceError (generation, "The calculation changed. Try again.");
                    return;
                }
                auto& store = MeshStore::Get ();
                const auto capture = store.ViewerCapture ();
                if (!store.IsCurrentCapture (capture, true)) {
                    ReportSurfaceError (generation,
                                        "Wait for the viewer model to finish loading or refresh the model.");
                    return;
                }
                GS::ObjectState params;
                params.Add ("sceneSnapshotId", GS::Int64 (capture->id));
                params.Add ("sceneCaptureStamp", GS::Int64 (capture->captureStamp));
                params.Add ("regionMode", GS::UniString (surfacetakeoff::RegionMode (action.scope)));
                const auto started = ExecuteNativeCommand ("StartCapturedExposedSurfaceTakeoff", params);
                if (!started.ok)
                    ReportSurfaceError (generation, started.error);
            },
            error))
        ReportSurfaceError (generation, error);
}

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
bool Refresh ();
// What keeps this host's main-thread HUD up: an editor document, or the Massing workflow.
bool Hosted (const overlayhud::State& state)
{
    return overlayhud::VirtualEditorActive (state) || s_massing;
}
bool AttachInput ()
{
    overlayinput::HudOwner owner { &Shown, &Refresh, &Redraw, &Hovering };
    std::string error;
    if (overlayinput::Attach (overlayinput::View::ThreeD, s_window, owner, error))
        return true;
    ArchVizLog ("VIEWER EDITOR input could not start: " + error);
    return false;
}

bool Refresh ()
{
    if (!Running () || s_refreshing)
        return false;
    if (!DiligentViewport::Get ().IsRunning ()) {
        Stop (true); // renderer failure must not silently discard the document
        return false;
    }
    // #201 Diagram Mode: this timer is the viewer's main-thread follow of the massing,
    // whether or not an editor workflow holds the input.
    diagrammodel::Follow ();
    FollowSurfaceTakeoff ();
    if (!Active ())
        viewportcursor::FollowHudTextFocus ();
    const auto state = guesttext::HudState ();
    const bool massing = s_massingRequested.exchange (false);
    if (s_requested.exchange (false) || massing) {
        {
            std::lock_guard<std::mutex> lock (s_mutex);
            if (s_placementRequested) {
                state->host.dock = s_dock;
                state->fontStep = s_fontStep;
                s_placementRequested = false;
            }
        }
        if (massing) {
            // The overlays' Massing page, held open with no editor document behind it.
            state->shown = true;
            state->host.open = true;
            state->host.selected = state->host.workflow = hudmassing::kTabKey;
            ++state->revision;
            if (Active () || AttachInput ()) {
                s_massing = true;
                s_active.store (true);
            }
        }
        else if (overlayhud::OpenVirtualEditorWorkflow (*state) && AttachInput ()) {
            overlayhud::RestoreVirtualEditor (*state, overlayhud::TakeVirtualEditor (*state));
            s_active.store (true);
        }
    }
    if (!Active ())
        return false;
    if (s_massing && overlayhud::VirtualEditorActive (*state) && state->host.workflow != hudmassing::kTabKey)
        s_massing = false; // the dock moved on to the editor: it holds the host now
    if (!Hosted (*state)) {
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
    // The Massing workflow reads the overlays' own pages and keeps the massing followed; the
    // editor alone keeps its smaller set. ⚠️ POLLED BEFORE THE ENGINE IS TAKEN: a poll can
    // publish the overlay layers, and nothing that runs after it may hold an engine pointer
    // taken before it (OverlayRelease.cpp ThreeD says why).
    if (s_massing) {
        massinghybrid::Poll ();
        massingslicesmodel::Poll ();
    }
    auto* engine = guesttext::Hud (overlayinput::View::ThreeD);
    if (!engine) {
        s_refreshing = false;
        return false;
    }
    overlayhud::OwnPages own =
        s_massing ? overlayhudmodel::Pages (overlayinput::View::ThreeD) : overlayhud::OwnPages {};
    own.standalone = true;
    own.viewerSurface = true;
    if (const auto highlight = ReadSurfaceHighlight ())
        own.exposedSurfaceHighlight = highlight->summary;
    own.overlay.tip = "Switch back to the Archicad overlay; keep this editor";
    own.viewer.active = true;
    own.viewer.phase = hudshell::Phase::Ready;
    own.viewer.tip = "Editing in the Tapioca 3D viewer";
    engine->SetOwnPages (std::move (own));
    const float scale = float (::GetDpiForWindow (s_window)) / 96.0f;
    auto drawing = std::make_unique<Drawing> ();
    drawing->scale = scale;
    drawing->hud = overlayscene::PrepareSceneHud (overlaylayers::Layers (), engine, scale, input);
    if (const auto action = overlayhud::TakeExposedSurfaceRequest (*state))
        RequestSurfaceTakeoff (*action);
    state->exposedSurfaceError = SurfaceTakeoffError ();
    overlayinput::EnableHudTextKeys (overlayhud::CriteriaTextActive (*state));
    // This host has no overlay-controller refresh. Drain its explicit file action
    // here on the main thread, after layout, never on the renderer's Present.
    if (state->virtualEditor && state->virtualEditor->debugExportRequested)
        edithud::SaveDebugExport (*state->virtualEditor);
    if (state->virtualEditor && state->virtualEditor->draftRequest)
        edithud::FollowDraftFiles (*state->virtualEditor); // #159 drafts, same rule
    FollowTerrainCapture (state);
    FollowDraftingImport (state);
    FollowFocus (state, false); // #124: the box goes to the render thread (RequestFocus)
    FollowMaterialPicker (state);
    if (s_massing && !overlayhudmodel::FollowWorkflowRequests (state)) {
        s_refreshing = false; // a modal let another project in: this turn drains nothing more
        return false;
    }
    if (engine->Selected () == "@viewer.analysis") {
        s_massing = false;
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
    if (!Hosted (*state)) {
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
void RequestMassingWorkflow (const hudshell::DockPlacement& dock, uint32_t fontStep)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_dock = dock;
    s_dock.workflowFlyoutOpen = false;
    s_fontStep = fontStep;
    s_placementRequested = true;
    s_massingRequested.store (true);
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
std::shared_ptr<const SurfaceHighlight> ReadSurfaceHighlight ()
{
    return s_surfaceHighlight.load ();
}
void RequestSurfaceTakeoff (surfacetakeoff::PageAction action)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    if (Running ()) {
        s_surfaceError.clear ();
        s_surfaceAction = action;
    }
}
std::string SurfaceTakeoffError ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_surfaceError;
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
void RequestFocus (const std::array<float, 3>& lo, const std::array<float, 3>& hi)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_focus = FocusBox { lo, hi };
}
bool TakeFocusRequest (FocusBox& box)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    if (!s_focus)
        return false;
    box = *std::exchange (s_focus, std::nullopt);
    return true;
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
    overlayinput::EnableHudTextKeys (false);
    viewportcursor::SetHudTextActive (false);
    s_requested.store (false);
    s_analysisRequested.store (false);
    s_massingRequested.store (false);
    s_massing = false;
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
        diagrammodel::Forget (); // a closed viewer ends Diagram Mode and restores the source
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
    s_surfaceUi = {};
    s_surfaceHighlight.store (nullptr);
    s_surfacePresentation = {};
    s_preparedGeneration = 0;
    s_surfaceAction.reset ();
    s_surfaceError.clear ();
}
} // namespace geomsrv::archviz::vieweredit
