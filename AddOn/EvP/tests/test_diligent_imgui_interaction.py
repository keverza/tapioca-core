from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARCHVIZ = ROOT / "Sources" / "AddOn" / "ArchViz"


def test_palette_drag_uses_win32_capture() -> None:
    panel = (ARCHVIZ / "ArchVizPanel.cpp").read_text(encoding="utf-8")

    assert "::SetCapture (hwnd)" in panel
    assert "::ReleaseCapture ()" in panel
    assert "capturedWindow" in panel


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
    assert "input -> ImGui frame" in lab
