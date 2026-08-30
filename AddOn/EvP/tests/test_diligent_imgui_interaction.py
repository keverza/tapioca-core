from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARCHVIZ = ROOT / "Sources" / "AddOn" / "ArchViz"


def test_palette_drag_uses_win32_capture() -> None:
    panel = (ARCHVIZ / "ArchVizPanel.cpp").read_text(encoding="utf-8")
    native_input = (ARCHVIZ / "ViewportCursor.cpp").read_text(encoding="utf-8")

    assert "::SetCapture (hwnd)" in native_input
    assert "::ReleaseCapture ()" in native_input
    assert "WM_CAPTURECHANGED" in native_input
    assert "PushButton" not in panel
    assert "PushWheel" not in panel


def test_palette_applies_low_latency_queue() -> None:
    viewport = (ARCHVIZ / "DiligentViewport.cpp").read_text(encoding="utf-8")
    target = (ARCHVIZ / "DiligentViewportTarget.cpp").read_text(encoding="utf-8")

    assert viewport.count("if (!offscreen)") >= 4
    assert viewport.count("ApplyRequestedFrameLatency") >= 3
    assert "IDXGIDevice1::SetMaximumFrameLatency" in target


def test_graph_lab_exercises_drag_and_edit_widgets() -> None:
    hud = (ARCHVIZ / "DiligentHud.cpp").read_text(encoding="utf-8")
    lab = (ARCHVIZ / "ImGuiGraphInteractionLab.cpp").read_text(encoding="utf-8")

    assert "input.inside || input.buttons != kMouseNone" in hud
    assert 'ImGui::Begin ("Number##graph-lab-number"' in lab
    assert "ImGui::DragFloat" in lab
    assert "ImGui::SliderFloat" in lab
    assert "ImGui::InputInt" in lab
    assert "ImGui::Checkbox" in lab
    assert "Graph canvas benchmark" in lab
    assert 'gesture = "node_drag"' in lab
    assert "wire_drag" in lab
    assert 'gesture = "pan"' in lab
    assert 'gesture = "zoom"' in lab


def test_graph_lab_logs_each_input_stage_without_swap_chain_hooks() -> None:
    native_input = (ARCHVIZ / "ViewportCursor.cpp").read_text(encoding="utf-8")
    hardware = (ARCHVIZ / "HardwareInput.cpp").read_text(encoding="utf-8")
    lab = (ARCHVIZ / "ImGuiGraphInteractionLab.cpp").read_text(encoding="utf-8")

    assert "WM_MOUSEMOVE" in native_input
    assert "PushPointerMessage" in native_input
    assert "QueryPerformanceCounter" in hardware
    assert "OS GetCursorPos" in lab
    assert "WM_MOUSEMOVE" in lab
    assert "ImGui consumed" in lab
    assert "60 Hz frame cadence" in lab
    assert "imgui_interaction.csv" in lab
    assert "host_style,host_exstyle,monitor" in lab
    assert "WH_GETMESSAGE" in native_input
    assert "PresentHook" not in native_input
    assert "PresentHook" not in lab
