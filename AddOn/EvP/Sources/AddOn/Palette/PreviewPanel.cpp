#include "Palette/PreviewPanel.hpp"

#include "Annotation/GdiPainter.hpp"
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/CameraSyncMode.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/HardwareInput.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SelectionBridge.hpp"
#include "PlanOverlay/PlanCanvasHost.hpp"
#include "Preview/PreviewRuntimeState.hpp"
#include "Preview/RetainedPreviewStore.hpp"
#include "Preview/RetainedTraceSelection.hpp"

#include "DGWin.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace evp {
namespace {

constexpr short ControlHeight = 22;
constexpr unsigned LeftButton = 0x01;
constexpr unsigned RightButton = 0x02;
constexpr unsigned MiddleButton = 0x04;

using previewpanel::Host;

geomsrv::archviz::CameraStart CameraForTrace (const geomsrv::annotation::DrawList& drawList)
{
    geomsrv::archviz::CameraStart camera;
    geomsrv::annotation::Point3 minimum;
    geomsrv::annotation::Point3 maximum;
    if (!drawList.GetBounds (minimum, maximum)) {
        camera.source = "retained trace has no drawable bounds";
        return camera;
    }

    const double center[3] = { (minimum.x + maximum.x) * 0.5, (minimum.y + maximum.y) * 0.5,
                               (minimum.z + maximum.z) * 0.5 };
    const double dx = maximum.x - minimum.x;
    const double dy = maximum.y - minimum.y;
    const double dz = maximum.z - minimum.z;
    const double radius = std::max (0.5, std::sqrt (dx * dx + dy * dy + dz * dz) * 0.5);
    constexpr double direction[3] = { 0.63, -0.67, 0.39 };
    for (int axis = 0; axis < 3; ++axis) {
        camera.target[axis] = float (center[axis]);
        camera.eye[axis] = float (center[axis] + direction[axis] * radius * 3.2);
    }
    camera.viewConeDegreesHorizontal = 50.0f;
    camera.valid = true;
    camera.source = "retained trace bounds";
    return camera;
}

} // namespace

PreviewPanel::PreviewPanel (const DG::Panel& panelRef, DG::CheckItemObserver& checkObserverRef,
                            DG::ButtonItemObserver& buttonObserverRef, DG::PopUpObserver& popupObserverRef,
                            DG::ScrollBarObserver& scrollObserverRef, DG::UserItemObserver& userItemObserverRef)
    : panel (panelRef), checkObserver (checkObserverRef), buttonObserver (buttonObserverRef),
      popupObserver (popupObserverRef), scrollObserver (scrollObserverRef), userItemObserver (userItemObserverRef)
{
}

PreviewPanel::~PreviewPanel ()
{
    void* const canvasWindow = CanvasWindow ();
    ReleaseMouseInput ();
    if (host.current == Host::Band) {
        geomsrv::archviz::DiligentViewport::Get ().Stop ();
        geomsrv::archviz::InputRingBuffer::Get ().Reset ();
    }
    geomsrv::archviz::ForgetHardwareInputWindow (canvasWindow);
    ClearOverlay ();
}

void PreviewPanel::Create ()
{
    const DG::Rect seed (0, 0, 100, ControlHeight);
    canvas = std::make_unique<DG::UserItem> (panel, DG::Rect (0, 0, 100, 100), DG::UserItem::Normal,
                                             DG::UserItem::StaticFrame);
    canvas->Attach (userItemObserver);
    canvas->EnableMouseMoveEvent ();
    canvas->EnableCapture ();
    enablePreviews = std::make_unique<DG::CheckBox> (panel, seed);
    enablePreviews->SetText ("Enable previews");
    enablePreviews->Attach (checkObserver);
    if (preview::PreviewRuntimeState::Get ().IsEnabled ())
        enablePreviews->Check ();
    nodeSelector = std::make_unique<DG::PopUp> (panel, seed, ControlHeight, 0);
    nodeSelector->Attach (popupObserver);
    frameScrubber = std::make_unique<DG::ScrollBar> (panel, seed, DG::ScrollBar::Normal, DG::ScrollBar::Focusable,
                                                     DG::ScrollBar::NoAutoScroll);
    frameScrubber->Attach (scrollObserver);
    frameScrubber->SetPageSize (1);
    frameLabel = std::make_unique<DG::LeftText> (panel, seed);

    overlayButton = std::make_unique<DG::Button> (panel, seed);
    popOutButton = std::make_unique<DG::Button> (panel, seed);
    returnButton = std::make_unique<DG::Button> (panel, seed);
    hideButton = std::make_unique<DG::Button> (panel, seed);
    for (DG::Button* button : { overlayButton.get (), popOutButton.get (), returnButton.get (), hideButton.get () })
        button->Attach (buttonObserver);
    overlayButton->SetText ("Overlay");
    popOutButton->SetText ("Pop out");
    popOutButton->Disable (); // plan2d has no independent popup host
    returnButton->SetText ("Return");
    hideButton->SetText ("Hide");
    UpdateLabels ();
}

short PreviewPanel::Height () const
{
    if (!active)
        return 0;
    return (short) previewpanel::BuildLayout (0, 300, 0, IsEnabled (), collapsed || host.CanvasCollapsed ()).height;
}

void PreviewPanel::SetKind (const GS::UniString& kind)
{
    ReleaseMouseInput ();
    const bool was3D = this->kind == "3d";
    this->kind = kind;
    active = kind == "plan2d" || kind == "3d";
    if (active && IsEnabled ()) {
        const auto snapshot = preview::RetainedPreviewStore::Get ().WatchTraceSnapshotCopy ();
        freshAfterGeneration = snapshot ? snapshot->generation : 0;
        retainedGeneration = freshAfterGeneration;
    }
    ClearTrace ();
    if (kind == "3d") {
        collapsed = false;
        Request3DHost (Host::None);
    }
    else if (kind != "3d" && was3D)
        Request3DHost (Host::None);
    ClearOverlay ();
    UpdateLabels ();
}

void PreviewPanel::SetEnabled (bool enabled)
{
    const bool wasEnabled = IsEnabled ();
    preview::PreviewRuntimeState::Get ().SetEnabled (enabled);
    if (enablePreviews) {
        if (enabled)
            enablePreviews->Check ();
        else
            enablePreviews->Uncheck ();
    }
    if (enabled) {
        const auto snapshot = preview::RetainedPreviewStore::Get ().WatchTraceSnapshotCopy ();
        freshAfterGeneration = snapshot ? snapshot->generation : 0;
        retainedGeneration = freshAfterGeneration;
    }
    if (wasEnabled && !enabled)
        preview::RetainedPreviewStore::Get ().ClearWatchSelection ();
    ClearTrace ();
    ClearOverlay ();
    if (!enabled) {
        ReleaseMouseInput ();
        Request3DHost (Host::None);
    }
    UpdateLabels ();
}

bool PreviewPanel::IsEnabled () const
{
    return preview::PreviewRuntimeState::Get ().IsEnabled ();
}

void PreviewPanel::SetPaletteVisible (bool visible)
{
    paletteVisible = visible;
    if (!visible)
        ReleaseMouseInput ();
    if (!visible && (host.current == Host::Band || host.target == Host::Band))
        Request3DHost (Host::None);
}

void PreviewPanel::PlaceAt (short left, short right, short bottom)
{
    if (!canvas)
        return;
    if (!active) {
        ReleaseMouseInput ();
        canvas->Hide ();
        enablePreviews->Hide ();
        nodeSelector->Hide ();
        frameScrubber->Hide ();
        frameLabel->Hide ();
        overlayButton->Hide ();
        popOutButton->Hide ();
        returnButton->Hide ();
        hideButton->Hide ();
        return;
    }
    const previewpanel::Layout layout =
        previewpanel::BuildLayout (left, right, bottom, IsEnabled (), collapsed || host.CanvasCollapsed ());
    const auto rect = [] (const previewpanel::Rect& value) {
        return DG::Rect ((short) value.left, (short) value.top, (short) value.right, (short) value.bottom);
    };
    enablePreviews->SetRect (rect (layout.enableControl));
    enablePreviews->Show ();
    if (!IsEnabled ()) {
        ReleaseMouseInput ();
        canvas->Hide ();
        nodeSelector->Hide ();
        frameScrubber->Hide ();
        frameLabel->Hide ();
        overlayButton->Hide ();
        popOutButton->Hide ();
        returnButton->Hide ();
        hideButton->Hide ();
        return;
    }
    overlayButton->SetRect (rect (layout.overlayButton));
    popOutButton->SetRect (rect (layout.popOutButton));
    returnButton->SetRect (rect (layout.returnButton));
    hideButton->SetRect (rect (layout.hideButton));
    for (DG::Button* button : { overlayButton.get (), popOutButton.get (), returnButton.get (), hideButton.get () })
        button->Show ();

    if (!layout.showCanvas) {
        ReleaseMouseInput ();
        canvas->Hide ();
        nodeSelector->Hide ();
        frameScrubber->Hide ();
        frameLabel->Hide ();
        return;
    }

    canvas->SetRect (rect (layout.canvas));
    if (kind == "plan2d") {
        planCamera.SetViewport (layout.canvas.Width (), layout.canvas.Height ());
        if (planFitPending && planCamera.Fit ())
            planFitPending = false;
    }
    nodeSelector->SetRect (rect (layout.nodeSelector));
    frameScrubber->SetRect (rect (layout.scrubber));
    frameLabel->SetRect (rect (layout.frameLabel));
    canvas->Show ();
    nodeSelector->Show ();
    frameScrubber->Show ();
    frameLabel->Show ();

    if (kind == "3d" && host.current == Host::Band) {
        HWND hwnd = DGGetDialogItemWindow (panel.GetId (), canvas->GetId ());
        RECT client {};
        if (hwnd != nullptr && ::IsWindow (hwnd) && ::GetClientRect (hwnd, &client) && client.right > 0 &&
            client.bottom > 0) {
            canvas->Redraw ();
            geomsrv::archviz::DiligentViewport::Get ().RequestResize (uint32_t (client.right),
                                                                      uint32_t (client.bottom));
        }
    }
}

bool PreviewPanel::PollRetained ()
{
    bool changed = false;
    if (IsEnabled ()) {
        const auto snapshot = preview::RetainedPreviewStore::Get ().WatchTraceSnapshotCopy ();
        if (snapshot && snapshot->generation > freshAfterGeneration && snapshot->generation != retainedGeneration) {
            retainedGeneration = snapshot->generation;
            drawList = preview::ToDrawList (snapshot->trace);
            selectedNode = selectedFrame = 0;
            planCameraSelectionValid = false;
            RebuildNodes ();
            const bool haveFrame = SelectFirstAvailableFrame ();
            if (!haveFrame) {
                preview::RetainedPreviewStore::Get ().ClearWatchSelection ();
                UpdateLabels ();
                if (canvas)
                    canvas->Redraw ();
            }
            changed = true;
            if (kind == "3d" && haveFrame && paletteVisible && !collapsed)
                Request3DHost (Host::Band);
            if (kind == "3d" && haveFrame && host.current == Host::Band && !host.transition &&
                retainedGeneration != presentedGeneration) {
                const geomsrv::archviz::CameraStart camera = CameraForTrace (drawList);
                if (camera.valid)
                    geomsrv::archviz::DiligentViewport::Get ().AdoptCamera (camera);
                presentedGeneration = retainedGeneration;
            }
        }
    }
    const previewpanel::HostState beforeHostPoll = host;
    Poll3DHost ();
    return changed || beforeHostPoll.current != host.current || beforeHostPoll.target != host.target ||
           beforeHostPoll.transition != host.transition;
}

void PreviewPanel::RebuildNodes ()
{
    nodeSelector->DeleteItem (DG::PopUp::AllItems);
    for (size_t index = 0; index < drawList.nodes.size (); ++index) {
        nodeSelector->AppendItem ();
        nodeSelector->SetItemText ((short) (index + 1), GS::UniString (drawList.nodes[index].name.c_str (), CC_UTF8));
    }
    frameScrubber->SetMin (0);
    frameScrubber->SetMax (0);
    frameScrubber->SetValue (0);
}

bool PreviewPanel::SelectFirstAvailableFrame ()
{
    for (size_t nodeIndex = 0; nodeIndex < drawList.nodes.size (); ++nodeIndex) {
        if (drawList.nodes[nodeIndex].frames.empty ())
            continue;
        selectedNode = nodeIndex;
        selectedFrame = 0;
        nodeSelector->SelectItem ((short) (nodeIndex + 1));
        frameScrubber->SetMax ((Int32) (drawList.nodes[nodeIndex].frames.size () - 1));
        SelectFrame (0);
        return true;
    }
    return false;
}

void PreviewPanel::ClearTrace ()
{
    drawList = {};
    planCamera.SetBounds ({});
    planFitPending = false;
    planCameraSelectionValid = false;
    selectedNode = selectedFrame = 0;
    if (IsEnabled ())
        preview::RetainedPreviewStore::Get ().ClearWatchSelection ();
    if (nodeSelector) {
        RebuildNodes ();
        UpdateLabels ();
        canvas->Redraw ();
    }
}

void PreviewPanel::SelectFrame (size_t frameIndex)
{
    if (!IsEnabled ())
        return;
    if (selectedNode >= drawList.nodes.size () || frameIndex >= drawList.nodes[selectedNode].frames.size ())
        return;
    selectedFrame = frameIndex;
    drawList.SelectFrame (selectedNode, selectedFrame);
    if (kind == "plan2d" &&
        (!planCameraSelectionValid || planCameraNode != selectedNode || planCameraFrame != selectedFrame)) {
        FitSelectedPlanFrame ();
        planCameraSelectionValid = true;
        planCameraNode = selectedNode;
        planCameraFrame = selectedFrame;
    }
    preview::RetainedPreviewStore::Get ().SelectWatchFrame (selectedNode, selectedFrame);
    frameScrubber->SetValue ((Int32) selectedFrame);
    UpdateLabels ();
    canvas->Redraw ();
    if (overlayActive)
        PublishOverlay ();
}

void PreviewPanel::UpdateLabels ()
{
    if (!IsEnabled ()) {
        for (DG::Button* button : { overlayButton.get (), popOutButton.get (), returnButton.get (), hideButton.get () })
            button->Disable ();
        return;
    }
    if (kind == "3d") {
        overlayButton->SetText ("3D Overlay");
        popOutButton->SetText ("Pop out");
        hideButton->SetText (collapsed ? "Show" : "Hide");
        if (host.transition) {
            overlayButton->Disable ();
            popOutButton->Disable ();
            hideButton->Disable ();
        }
        else {
            if (drawList.SelectedFrame () != nullptr) {
                overlayButton->Enable ();
                popOutButton->Enable ();
            }
            else {
                overlayButton->Disable ();
                popOutButton->Disable ();
            }
            hideButton->Enable ();
        }
        if (host.ExternalStartingOrActive ())
            returnButton->Enable ();
        else
            returnButton->Disable ();
        return;
    }

    overlayButton->SetText (overlayActive ? "Overlay on" : "Overlay");
    popOutButton->Disable ();
    if (overlayActive)
        returnButton->Enable ();
    else
        returnButton->Disable ();
    hideButton->Enable ();
    const geomsrv::annotation::Frame* frame = drawList.SelectedFrame ();
    if (!frame) {
        frameLabel->SetText ("No trace");
        return;
    }
    const size_t count = drawList.SelectedNode ()->frames.size ();
    frameLabel->SetText (GS::UniString::Printf ("%u/%u", (unsigned) (selectedFrame + 1), (unsigned) count));
}

void PreviewPanel::SetPresentationStatus (const char* status)
{
    if (frameLabel != nullptr)
        frameLabel->SetText (status);
}

void PreviewPanel::Request3DHost (Host requestedHost)
{
    if (!IsEnabled () && requestedHost != Host::None)
        return;
    if (requestedHost != Host::None && drawList.SelectedFrame () == nullptr)
        return;
    if (host.transition) {
        if (host.target != requestedHost) {
            host.target = requestedHost;
            UpdateLabels ();
        }
        return;
    }
    host.target = requestedHost;
    if (host.current == host.target && !host.transition)
        return;

    host.transition = true;
    if (host.current == Host::Band) {
        ReleaseMouseInput ();
        SetPresentationStatus ("Stopping 3D...");
        geomsrv::archviz::DiligentViewport::Get ().RequestStop ();
    }
    else if (host.current == Host::Overlay || host.current == Host::PopOut) {
        SetPresentationStatus ("Closing 3D host...");
        geomsrv::archviz::ShutDownCameraSync ();
        geomsrv::archviz::modelwatch::Stop ();
        geomsrv::archviz::selectionbridge::Stop ();
        geomsrv::archviz::ExtractionWorker::Get ().RequestStop ();
        geomsrv::archviz::DiligentViewport::Get ().RequestStop ();
    }
    else {
        SetPresentationStatus (requestedHost == Host::Band      ? "Starting 3D..."
                               : requestedHost == Host::Overlay ? "Opening overlay..."
                               : requestedHost == Host::PopOut  ? "Opening pop-out..."
                                                                : "Hidden");
    }
    UpdateLabels ();
}

bool PreviewPanel::StartBandHost ()
{
    if (!IsEnabled () || !paletteVisible || drawList.SelectedFrame () == nullptr)
        return false;
    if (geomsrv::archviz::ExtractionWorker::Get ().IsRunning ()) {
        SetPresentationStatus ("3D busy extracting");
        return false;
    }
    HWND hwnd = DGGetDialogItemWindow (panel.GetId (), canvas != nullptr ? canvas->GetId () : 0);
    RECT client {};
    if (hwnd == nullptr || ::IsWindow (hwnd) == FALSE || ::GetClientRect (hwnd, &client) == FALSE ||
        client.right <= 0 || client.bottom <= 0) {
        SetPresentationStatus ("3D surface unavailable");
        return false;
    }

    geomsrv::archviz::Surface surface;
    surface.nwh = hwnd;
    surface.width = uint32_t (client.right);
    surface.height = uint32_t (client.bottom);
    surface.retainedAnnotationsOnly = true;
    // Close the global middle-button path before the render thread starts. A
    // press over this canvas will reopen it after DG has aimed the event.
    geomsrv::archviz::SetHardwareInputEnabled (hwnd, false);
    geomsrv::archviz::SceneCmdQueue::Get ().Clear ();
    const geomsrv::archviz::CameraStart camera = CameraForTrace (drawList);
    if (!geomsrv::archviz::DiligentViewport::Get ().Start (surface, camera)) {
        SetPresentationStatus ("3D start refused");
        return false;
    }
    host.current = Host::Band;
    inputState.SetAvailable (true);
    RefreshPointerInput ();
    presentedGeneration = retainedGeneration;
    SetPresentationStatus ("Starting 3D...");
    return true;
}

void PreviewPanel::FinishExternalHost ()
{
    // Both workers have already published stopped, so these safety joins return
    // immediately while the existing hosts perform their normal HWND teardown.
    geomsrv::archviz::ExtractionWorker::Get ().Stop ();
    if (host.current == Host::Overlay)
        ArchVizPanel::CloseDiligentOverlay ();
    else if (host.current == Host::PopOut)
        ArchVizPanel::CloseViewer ();
    host.current = Host::None;
}

void PreviewPanel::Poll3DHost ()
{
    if (kind != "3d" && host.current == Host::None && !host.transition)
        return;

    auto& viewport = geomsrv::archviz::DiligentViewport::Get ();
    if (!host.transition && (host.current == Host::Overlay || host.current == Host::PopOut) && !viewport.IsRunning ()) {
        FinishExternalHost ();
        host.target = Host::None;
        SetPresentationStatus ("External host closed");
        UpdateLabels ();
        return;
    }
    if (host.transition && host.current != Host::None) {
        const bool extractionStopped =
            host.current == Host::Band || !geomsrv::archviz::ExtractionWorker::Get ().IsRunning ();
        if (viewport.IsRunning () || !extractionStopped)
            return;
        if (host.current == Host::Band) {
            void* const canvasWindow = CanvasWindow ();
            viewport.Stop ();
            geomsrv::archviz::InputRingBuffer::Get ().Reset ();
            geomsrv::archviz::ForgetHardwareInputWindow (canvasWindow);
            host.current = Host::None;
        }
        else {
            FinishExternalHost ();
        }
    }

    if (host.transition && host.current == Host::None) {
        bool started = true;
        if (host.target == Host::Band)
            started = StartBandHost ();
        else if (host.target == Host::Overlay) {
            ArchVizPanel::OpenDiligentOverlay ();
            started = viewport.IsRunning () && viewport.Mode () == geomsrv::archviz::SurfaceMode::Overlay;
            if (started) {
                host.current = Host::Overlay;
                SetPresentationStatus ("3D overlay");
            }
        }
        else if (host.target == Host::PopOut) {
            ArchVizPanel::OpenDiligentViewport ();
            started = viewport.IsRunning () && viewport.Mode () == geomsrv::archviz::SurfaceMode::PaletteChild;
            if (started) {
                host.current = Host::PopOut;
                SetPresentationStatus ("3D pop-out");
            }
        }
        else {
            SetPresentationStatus ("Hidden");
        }
        host.transition = false;
        if (!started)
            host.target = Host::None;
        UpdateLabels ();
        return;
    }

    if (host.current == Host::Band) {
        const geomsrv::archviz::DiligentViewportStats stats = viewport.Stats ();
        if (stats.failed)
            SetPresentationStatus ("3D failed");
        else if (!stats.initialized)
            SetPresentationStatus ("Starting 3D...");
        else
            SetPresentationStatus ("3D on band");
    }
}

void PreviewPanel::PublishOverlay ()
{
    if (!IsEnabled ())
        return;
    const geomsrv::annotation::Frame* frame = drawList.SelectedFrame ();
    if (frame == nullptr) {
        ClearOverlay ();
        overlayButton->SetText ("No trace");
        return;
    }

    geomsrv::planoverlay::Style style;
    const geomsrv::planoverlay::SessionStart started =
        geomsrv::planoverlay::BeginCurrentPlanSession (geomsrv::planoverlay::Owner::Watch, style, 33, overlaySession);
    if (started != geomsrv::planoverlay::SessionStart::Opened &&
        started != geomsrv::planoverlay::SessionStart::Reused) {
        overlayActive = false;
        if (started == geomsrv::planoverlay::SessionStart::NotFloorPlan)
            overlayButton->SetText ("Open floor plan");
        else if (started == geomsrv::planoverlay::SessionStart::OverlayInUse)
            overlayButton->SetText ("Overlay in use");
        else
            overlayButton->SetText ("Overlay unavailable");
        return;
    }
    geomsrv::planoverlay::SetAnnotationFrame (std::make_shared<geomsrv::annotation::Frame> (*frame));
    overlayActive = true;
    overlayButton->SetText ("Overlay on");
}

void PreviewPanel::ClearOverlay ()
{
    if (!overlayActive && overlaySession.window == nullptr)
        return;
    geomsrv::planoverlay::SetAnnotationFrame (nullptr);
    geomsrv::planoverlay::EndSession (overlaySession);
    overlayActive = false;
    overlayButton->SetText ("Overlay");
}

bool PreviewPanel::HandleCheckItemChanged (const DG::CheckItemChangeEvent& event)
{
    if (!enablePreviews || event.GetSource () != enablePreviews.get ())
        return false;
    SetEnabled (enablePreviews->IsChecked ());
    return true;
}

bool PreviewPanel::HandleButtonClicked (const DG::ButtonClickEvent& event)
{
    if (!IsEnabled ())
        return event.GetSource () == overlayButton.get () || event.GetSource () == popOutButton.get () ||
               event.GetSource () == returnButton.get () || event.GetSource () == hideButton.get ();
    if (event.GetSource () == overlayButton.get ()) {
        if (kind == "3d") {
            Request3DHost (Host::Overlay);
            return true;
        }
        if (overlayActive)
            ClearOverlay ();
        else
            PublishOverlay ();
        return true;
    }
    if (event.GetSource () == popOutButton.get ()) {
        if (kind == "3d") {
            Request3DHost (Host::PopOut);
        }
        return true;
    }
    if (event.GetSource () == returnButton.get ()) {
        ReleaseMouseInput ();
        if (kind == "3d") {
            collapsed = false;
            if (drawList.SelectedFrame () != nullptr && paletteVisible)
                Request3DHost (Host::Band);
            return true;
        }
        ClearOverlay ();
        collapsed = false;
        hideButton->SetText ("Hide");
        canvas->Show ();
        nodeSelector->Show ();
        frameScrubber->Show ();
        frameLabel->Show ();
        canvas->Redraw ();
        return true;
    }
    if (event.GetSource () == hideButton.get ()) {
        ReleaseMouseInput ();
        if (kind == "3d") {
            collapsed = !collapsed;
            Request3DHost (collapsed || drawList.SelectedFrame () == nullptr ? Host::None : Host::Band);
            return true;
        }
        ClearOverlay ();
        collapsed = !collapsed;
        hideButton->SetText (collapsed ? "Show" : "Hide");
        if (collapsed) {
            canvas->Hide ();
            nodeSelector->Hide ();
            frameScrubber->Hide ();
            frameLabel->Hide ();
        }
        else {
            canvas->Show ();
            nodeSelector->Show ();
            frameScrubber->Show ();
            frameLabel->Show ();
            canvas->Redraw ();
        }
        return true;
    }
    return false;
}

bool PreviewPanel::HandlePopUpChanged (const DG::PopUpChangeEvent& event)
{
    if (event.GetSource () != nodeSelector.get ())
        return false;
    const short item = nodeSelector->GetSelectedItem ();
    if (item < 1 || (size_t) item > drawList.nodes.size ())
        return true;
    selectedNode = (size_t) item - 1;
    selectedFrame = 0;
    const size_t count = drawList.nodes[selectedNode].frames.size ();
    frameScrubber->SetMax ((Int32) (count > 0 ? count - 1 : 0));
    if (count > 0)
        SelectFrame (0);
    else {
        drawList.SelectNode (selectedNode);
        preview::RetainedPreviewStore::Get ().ClearWatchSelection ();
        UpdateLabels ();
        canvas->Redraw ();
    }
    return true;
}

bool PreviewPanel::HandleScrollBarChanged (const DG::ScrollBarChangeEvent& event)
{
    if (event.GetSource () != frameScrubber.get ())
        return false;
    SelectFrame ((size_t) frameScrubber->GetValue ());
    return true;
}

bool PreviewPanel::HandleScrollBarTracked (const DG::ScrollBarTrackEvent& event)
{
    if (event.GetSource () != frameScrubber.get ())
        return false;
    SelectFrame ((size_t) frameScrubber->GetValue ());
    return true;
}

bool PreviewPanel::HandleUserItemUpdate (const DG::UserItemUpdateEvent& event)
{
    if (!canvas || event.GetSource () != canvas.get ())
        return false;
    if (kind == "3d") {
        const geomsrv::archviz::DiligentViewportStats stats = geomsrv::archviz::DiligentViewport::Get ().Stats ();
        if (host.current != Host::Band || !stats.running || stats.width != (uint32_t) canvas->GetWidth () ||
            stats.height != (uint32_t) canvas->GetHeight ()) {
            HDC hdc = static_cast<HDC> (event.GetDrawContext ());
            RECT rect { 0, 0, canvas->GetWidth (), canvas->GetHeight () };
            HBRUSH neutral = CreateSolidBrush (RGB (238, 238, 238));
            FillRect (hdc, &rect, neutral);
            DeleteObject (neutral);
        }
        return true;
    }
    HDC hdc = static_cast<HDC> (event.GetDrawContext ());
    RECT rect { 0, 0, canvas->GetWidth (), canvas->GetHeight () };
    FillRect (hdc, &rect, static_cast<HBRUSH> (GetStockObject (WHITE_BRUSH)));
    const geomsrv::annotation::Frame* frame = drawList.SelectedFrame ();
    if (frame)
        geomsrv::annotation::PaintFrameGdi (hdc, *frame, planCamera.Transform ());
    return true;
}

bool PreviewPanel::HandleUserItemMouseDown (const DG::UserItemMouseDownEvent& event)
{
    if (PlanInputAvailable () && event.GetSource () == canvas.get ()) {
        if (!event.IsWheelButton ())
            return false;
        const DG::Point point = event.GetMouseOffset ();
        const previewpanel::PlanPointerAction action =
            planCamera.MiddleDown (point.GetX (), point.GetY (), ::GetTickCount64 ());
        if (action == previewpanel::PlanPointerAction::Fitted)
            canvas->Redraw ();
        HWND const hwnd = static_cast<HWND> (CanvasWindow ());
        if (planCamera.IsCaptured () && hwnd != nullptr) {
            ::SetCapture (hwnd);
            if (::GetCapture () == hwnd)
                capturedWindow = hwnd;
        }
        return action != previewpanel::PlanPointerAction::None;
    }
    if (!EmbeddedInputAvailable () || event.GetSource () != canvas.get () || !RefreshPointerInput ())
        return false;
    unsigned pressed = 0;
    if (event.IsLeftButton ())
        pressed |= LeftButton;
    if (event.IsRightButton ())
        pressed |= RightButton;
    if (event.IsWheelButton ())
        pressed |= MiddleButton;
    if (!inputState.Press (pressed))
        return false;

    uint8_t button = geomsrv::archviz::kMouseNone;
    if (event.IsLeftButton ())
        button = geomsrv::archviz::kMouseLeft;
    else if (event.IsRightButton ())
        button = geomsrv::archviz::kMouseRight;
    if (button != geomsrv::archviz::kMouseNone)
        geomsrv::archviz::InputRingBuffer::Get ().PushButton (button, true);

    HWND const hwnd = static_cast<HWND> (CanvasWindow ());
    if (hwnd != nullptr && ::IsWindow (hwnd)) {
        ::SetCapture (hwnd);
        if (::GetCapture () == hwnd)
            capturedWindow = hwnd;
    }
    return true;
}

bool PreviewPanel::HandleUserItemMouseUp (const DG::UserItemMouseUpEvent& event)
{
    if (PlanInputAvailable () && event.GetSource () == canvas.get () && event.IsWheelButton ()) {
        planCamera.EndPan ();
        if (capturedWindow != nullptr && ::GetCapture () == static_cast<HWND> (capturedWindow))
            ::ReleaseCapture ();
        capturedWindow = nullptr;
        return true;
    }
    if (!EmbeddedInputAvailable () || event.GetSource () != canvas.get ())
        return false;
    unsigned released = 0;
    if (event.IsLeftButton ())
        released |= LeftButton;
    if (event.IsRightButton ())
        released |= RightButton;
    if (event.IsWheelButton ())
        released |= MiddleButton;
    if (released == 0)
        released = LeftButton | RightButton | MiddleButton;
    inputState.Release (released);
    uint8_t button = geomsrv::archviz::kMouseNone;
    if (event.IsLeftButton ())
        button = geomsrv::archviz::kMouseLeft;
    else if (event.IsRightButton ())
        button = geomsrv::archviz::kMouseRight;
    if (button == geomsrv::archviz::kMouseNone && !event.IsWheelButton ())
        button = uint8_t (geomsrv::archviz::kMouseLeft | geomsrv::archviz::kMouseRight);
    if (button != geomsrv::archviz::kMouseNone)
        geomsrv::archviz::InputRingBuffer::Get ().PushButton (button, false);
    if (!inputState.IsDragging () && capturedWindow != nullptr) {
        if (::GetCapture () == static_cast<HWND> (capturedWindow))
            ::ReleaseCapture ();
        capturedWindow = nullptr;
    }
    return true;
}

bool PreviewPanel::HandleUserItemDoubleClicked (const DG::UserItemDoubleClickEvent& event)
{
    if (!PlanInputAvailable () || event.GetSource () != canvas.get () || !event.IsWheelButton ())
        return false;
    if (planCamera.DoubleClickFit ())
        canvas->Redraw ();
    if (capturedWindow != nullptr && ::GetCapture () == static_cast<HWND> (capturedWindow))
        ::ReleaseCapture ();
    capturedWindow = nullptr;
    return true;
}

bool PreviewPanel::HandleUserItemMouseEntered (const DG::UserItemMouseEnteredEvent& event)
{
    if (PlanInputAvailable () && event.GetSource () == canvas.get ())
        return true;
    if (!EmbeddedInputAvailable () || event.GetSource () != canvas.get ())
        return false;
    RefreshPointerInput ();
    return true;
}

bool PreviewPanel::HandleUserItemMouseMoved (const DG::UserItemMouseMoveEvent& event)
{
    if (PlanInputAvailable () && event.GetSource () == canvas.get ()) {
        if (planCamera.IsCaptured ()) {
            const DG::Point point = event.GetMouseOffset ();
            if (planCamera.PanTo (point.GetX (), point.GetY ()))
                canvas->Redraw ();
        }
        return true;
    }
    if (!EmbeddedInputAvailable () || event.GetSource () != canvas.get ())
        return false;
    if (!RefreshPointerInput ())
        ReleaseMouseInput ();
    return true;
}

bool PreviewPanel::HandleUserItemMouseExited (const DG::UserItemMouseExitedEvent& event)
{
    if (!canvas || event.GetSource () != canvas.get ())
        return false;
    if (PlanInputAvailable () && planCamera.IsCaptured ())
        return true;
    ReleaseMouseInput ();
    return true;
}

bool PreviewPanel::HandleWheelTracked (const DG::PanelWheelTrackEvent& event)
{
    if (PlanInputAvailable () && event.GetItem () == canvas.get ()) {
        geomsrv::archviz::HardwarePointerPosition pointer;
        if (!geomsrv::archviz::ReadHardwarePointer (CanvasWindow (), pointer) || !pointer.inside)
            return false;
        if (planCamera.ZoomAt (pointer.x, pointer.y, event.GetYTrackValue ()))
            canvas->Redraw ();
        return event.GetYTrackValue () != 0;
    }
    if (!EmbeddedInputAvailable () || !RefreshPointerInput ())
        return false;
    geomsrv::archviz::InputRingBuffer::Get ().PushWheel (event.GetYTrackValue ());
    return true;
}

bool PreviewPanel::EmbeddedInputAvailable () const
{
    return IsEnabled () && paletteVisible && kind == "3d" && canvas && host.current == Host::Band && !host.transition &&
           !collapsed;
}

bool PreviewPanel::PlanInputAvailable () const
{
    return IsEnabled () && paletteVisible && kind == "plan2d" && canvas && !collapsed;
}

void PreviewPanel::FitSelectedPlanFrame ()
{
    geomsrv::annotation::Point3 minimum;
    geomsrv::annotation::Point3 maximum;
    const geomsrv::annotation::Frame* const frame = drawList.SelectedFrame ();
    if (frame == nullptr || !geomsrv::annotation::GetBounds (*frame, minimum, maximum)) {
        planCamera.SetBounds ({});
        planFitPending = false;
        return;
    }
    planCamera.SetBounds ({ minimum.x, minimum.y, maximum.x, maximum.y, true });
    planFitPending = !planCamera.Fit ();
}

void* PreviewPanel::CanvasWindow () const
{
    if (!canvas)
        return nullptr;
    HWND const hwnd = DGGetDialogItemWindow (panel.GetId (), canvas->GetId ());
    return hwnd != nullptr && ::IsWindow (hwnd) ? hwnd : nullptr;
}

bool PreviewPanel::RefreshPointerInput ()
{
    void* const hwnd = CanvasWindow ();
    geomsrv::archviz::HardwarePointerPosition pointer;
    const bool inside =
        EmbeddedInputAvailable () && geomsrv::archviz::ReadHardwarePointer (hwnd, pointer) && pointer.inside;
    inputState.SetAvailable (EmbeddedInputAvailable ());
    inputState.SetPointerInside (inside);
    geomsrv::archviz::SetHardwareInputEnabled (hwnd, inside);
    return inside;
}

void PreviewPanel::ReleaseMouseInput ()
{
    const bool embeddedHost = host.current == Host::Band;
    planCamera.Cancel ();
    inputState.ReleaseAll ();
    inputState.SetAvailable (false);
    if (embeddedHost) {
        geomsrv::archviz::InputRingBuffer::Get ().Reset ();
        geomsrv::archviz::SetHardwareInputEnabled (CanvasWindow (), false);
    }
    if (capturedWindow != nullptr && ::GetCapture () == static_cast<HWND> (capturedWindow))
        ::ReleaseCapture ();
    capturedWindow = nullptr;
}

bool PreviewPanel::IsCanvas (const DG::Item* item) const
{
    return canvas && item == canvas.get ();
}

} // namespace evp
