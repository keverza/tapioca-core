"""Native 3D preview host lifecycle and source-boundary contracts."""

from pathlib import Path

_ADDON = Path(__file__).parents[1] / "Sources" / "AddOn"
_PANEL = _ADDON / "Palette" / "PreviewPanel.cpp"
_VIEWPORT_CONTROL = _ADDON / "ArchViz" / "DiligentViewportControl.cpp"
_PLAN_HOST = _ADDON / "PlanOverlay" / "PlanCanvasHost.cpp"
_PLAN_OVERLAY = _ADDON / "PlanOverlay" / "OverlayWindow.cpp"
_PLAN_COMMANDS = _ADDON / "NativeCommands" / "PlanOverlayCommands.cpp"
_RUNTIME = _ADDON / "Preview" / "PreviewRuntimeState.hpp"


def test_3d_band_uses_validated_dg_hwnd_without_full_model_extraction():
    source = _PANEL.read_text(encoding="utf-8")
    start = source[source.index("bool PreviewPanel::StartBandHost") :]
    start = start[: start.index("void PreviewPanel::FinishExternalHost")]

    assert "DGGetDialogItemWindow" in start
    assert "::IsWindow" in start
    assert "::GetClientRect" in start
    assert start.index("SceneCmdQueue::Get ().Clear") < start.index(
        "DiligentViewport::Get ().Start"
    )
    assert "ExtractionWorker::Get ().Start" not in start
    assert "CameraForTrace (drawList)" in start
    assert "surface.retainedAnnotationsOnly = true" in start


def test_3d_handoffs_are_idle_driven_after_flag_only_stop_requests():
    panel = _PANEL.read_text(encoding="utf-8")
    viewport = _VIEWPORT_CONTROL.read_text(encoding="utf-8")

    assert "void DiligentViewport::RequestStop" in viewport
    assert "stopRequested_.store (true)" in viewport
    request = panel[panel.index("void PreviewPanel::Request3DHost") :]
    request = request[: request.index("bool PreviewPanel::StartBandHost")]
    assert "DiligentViewport::Get ().RequestStop" in request
    assert "DiligentViewport::Get ().Stop" not in request
    assert "Poll3DHost ();" in panel
    assert "ArchVizPanel::OpenDiligentOverlay" in panel
    assert "ArchVizPanel::OpenDiligentViewport" in panel
    assert "ArchVizPanel::CloseDiligentOverlay" in panel
    assert "ArchVizPanel::CloseViewer" in panel


def test_3d_band_routes_supported_user_item_input_to_existing_ring():
    source = _PANEL.read_text(encoding="utf-8")
    shell = (_ADDON / "Palette" / "ControlPaletteLayout.cpp").read_text(
        encoding="utf-8"
    )
    hardware = (_ADDON / "ArchViz" / "HardwareInput.cpp").read_text(
        encoding="utf-8"
    )

    assert "PreviewPanel::HandleUserItemMouseDown" in source
    assert "PreviewPanel::HandleUserItemMouseUp" in source
    assert "PreviewPanel::HandleUserItemMouseMoved" in source
    assert "PreviewPanel::HandleUserItemMouseExited" in source
    assert "PreviewPanel::HandleWheelTracked" in source
    assert "InputRingBuffer::Get ().PushButton" in source
    assert "InputRingBuffer::Get ().PushWheel" in source
    assert "canvas->EnableMouseMoveEvent ()" in source
    assert "canvas->EnableCapture ()" in source
    assert "::SetCapture (hwnd)" in source
    assert "::ReleaseCapture ()" in source
    assert "preview.HandleUserItemMouseMoved (ev)" in shell
    assert "::ScreenToClient (hwnd, &clientPt)" in hardware
    assert "DGGetDPI" not in hardware


def test_3d_band_releases_input_on_every_host_lifecycle_exit():
    source = _PANEL.read_text(encoding="utf-8")

    for signature in (
        "PreviewPanel::~PreviewPanel",
        "void PreviewPanel::SetKind",
        "void PreviewPanel::SetEnabled",
        "void PreviewPanel::SetPaletteVisible",
        "void PreviewPanel::Request3DHost",
        "bool PreviewPanel::HandleUserItemMouseExited",
    ):
        section = source[source.index(signature) :]
        assert "ReleaseMouseInput ();" in section
    assert "InputRingBuffer::Get ().Reset ();" in source
    assert "SetHardwareInputEnabled (CanvasWindow (), false)" in source


def test_renderer_consumes_only_retained_watch_selection_for_preview_annotations():
    renderer = (_ADDON / "ArchViz" / "DiligentViewport.cpp").read_text(
        encoding="utf-8"
    )

    assert "SelectedRetainedFrameSnapshotCopy" in renderer
    assert "BuildTraceAnnotations" in renderer
    assert "annotationsOnly ? 0 : scene.Consume" in renderer
    assert "!annotationsOnly && !blanked" in renderer


def test_plan_overlay_opens_through_shared_native_host_without_a_bus_command():
    panel = _PANEL.read_text(encoding="utf-8")
    publish = panel[panel.index("void PreviewPanel::PublishOverlay") :]
    publish = publish[: publish.index("void PreviewPanel::ClearOverlay")]
    host = _PLAN_HOST.read_text(encoding="utf-8")
    commands = _PLAN_COMMANDS.read_text(encoding="utf-8")

    assert "BeginCurrentPlanSession" in publish
    assert "SetAnnotationFrame" in publish
    assert "Current ()" not in publish
    assert "CallCommand" not in publish
    assert "ACAPI_Window_GetCurrentWindow" in host
    assert "APIWind_FloorPlanID" in host
    assert "DescendWindowChain" in host
    assert "std::vector<HWND> DescendWindowChain" not in commands
    assert "DescendWindowChain" in commands


def test_plan_overlay_uses_prompt_idle_and_fast_active_tracking_cadences():
    source = _PLAN_OVERLAY.read_text(encoding="utf-8")
    cadence = source[source.index("void SetTimerCadence") :]
    cadence = cadence[: cadence.index("LRESULT CALLBACK OverlayWndProc")]

    assert "TRACK_FAST_INTERVAL_MS = 16" in source
    assert "TRACK_STABLE_INTERVAL_MS = 33" in source
    assert "(std::min) (s_requestedIntervalMs, TRACK_FAST_INTERVAL_MS)" in cadence
    assert "(std::max) (s_requestedIntervalMs, TRACK_STABLE_INTERVAL_MS)" in cadence
    assert "SetTimerCadence (hwnd, torn || now - s_lastActivityTick < 250)" in source


def test_plan_overlay_return_releases_the_explicit_watch_session():
    panel = _PANEL.read_text(encoding="utf-8")
    clear = panel[panel.index("void PreviewPanel::ClearOverlay") :]
    clear = clear[: clear.index("bool PreviewPanel::HandleButtonClicked")]

    assert clear.index("SetAnnotationFrame (nullptr)") < clear.index(
        "EndSession (overlaySession)"
    )
    assert "Owner::Watch" in panel


def test_preview_enable_state_gates_palette_and_webui_watch_arming():
    palette_run = (_ADDON / "Palette" / "ControlPaletteRun.cpp").read_text(
        encoding="utf-8"
    )
    webui = (_ADDON / "Python" / "WebUIDispatch.cpp").read_text(encoding="utf-8")
    runtime = _RUNTIME.read_text(encoding="utf-8")
    native = (_ADDON / "NativeCommands" / "PreviewCommands.cpp").read_text(
        encoding="utf-8"
    )

    assert "std::atomic<bool> enabled" in runtime
    assert "PreviewRuntimeState::Get ().IsEnabled ()" in palette_run
    assert "PreviewRuntimeState::Get ().IsEnabled ()" in webui
    assert native.count("PreviewRuntimeState::Get ().IsEnabled ()") == 2
    assert native.count('Failure ("previews are disabled")') == 2


def test_fresh_trace_is_required_and_first_nonempty_node_is_selected():
    panel = _PANEL.read_text(encoding="utf-8")
    poll = panel[panel.index("bool PreviewPanel::PollRetained") :]
    poll = poll[: poll.index("void PreviewPanel::RebuildNodes")]
    first = panel[panel.index("bool PreviewPanel::SelectFirstAvailableFrame") :]
    first = first[: first.index("void PreviewPanel::ClearTrace")]

    assert "snapshot->generation > freshAfterGeneration" in poll
    assert poll.index("SelectFirstAvailableFrame") < poll.index("Request3DHost (Host::Band)")
    assert "drawList.nodes[nodeIndex].frames.empty ()" in first
    assert "SelectFrame (0)" in first


def test_palette_consumes_retained_change_for_layout_and_redraw():
    shell = (_ADDON / "Palette" / "ControlPalette.cpp").read_text(encoding="utf-8")
    idle = shell[shell.index("void ControlPalette::PanelIdle") :]
    idle = idle[: idle.index("void ControlPalette::RefreshSearchFilter")]
    automatic = (_ADDON / "Palette" / "ControlPaletteAutoPreview.cpp").read_text(
        encoding="utf-8"
    )

    assert "PollAutomaticPreview ();" in idle
    assert "if (!runActive.load () && preview.PollRetained ())" in automatic
    assert "Layout ();" in automatic
    assert "Redraw ();" in automatic


def test_band_resize_has_neutral_fallback_and_duplicate_requests_are_coalesced():
    panel = _PANEL.read_text(encoding="utf-8")
    viewport = (_ADDON / "ArchViz" / "DiligentViewportControl.cpp").read_text(
        encoding="utf-8"
    )

    assert "stats.width != (uint32_t) canvas->GetWidth ()" in panel
    assert "CreateSolidBrush (RGB (238, 238, 238))" in panel
    resize = viewport[viewport.index("void DiligentViewport::RequestResize") :]
    resize = resize[: resize.index("void DiligentViewport::SyncCamera")]
    assert "pendingWidth_.load () == width" in resize
    assert resize.index("pendingWidth_.load () == width") < resize.index(
        "resizePending_.store (true)"
    )


def test_plan_band_uses_persistent_camera_and_canvas_scoped_input():
    panel = _PANEL.read_text(encoding="utf-8")
    paint = panel[panel.index("bool PreviewPanel::HandleUserItemUpdate") :]
    paint = paint[: paint.index("bool PreviewPanel::HandleUserItemMouseDown")]
    select = panel[panel.index("void PreviewPanel::SelectFrame") :]
    select = select[: select.index("void PreviewPanel::UpdateLabels")]
    wheel = panel[panel.index("bool PreviewPanel::HandleWheelTracked") :]
    wheel = wheel[: wheel.index("bool PreviewPanel::EmbeddedInputAvailable")]

    assert "planCamera.Transform ()" in paint
    assert "FitFrame" not in paint
    assert "planCameraSelectionValid" in select
    assert 'event.GetItem () == canvas.get ()' in wheel
    assert "planCamera.ZoomAt" in wheel
    assert "HandleUserItemDoubleClicked" in panel
