"""SDK/runtime seams for the roof canvas; these contracts are not live acceptance."""

from pathlib import Path

ROOT = Path(__file__).parents[1] / "Sources/AddOn/ArchViz"


def test_roof_numbers_stay_inline_and_do_not_open_native_prompts():
    controller = (ROOT / "OverlayControllerHud.cpp").read_text(encoding="utf-8")
    canvas = (ROOT / "RoofSlopeHud.cpp").read_text(encoding="utf-8")
    assert "TakeRoofNumber" not in controller
    assert 'ImGui::InputText ("##roofNumber"' in canvas
    assert "ImGuiKey_Escape" in canvas


def test_input_turn_publishes_plan_edits_without_waiting_for_tick():
    source = (ROOT / "PlanOverlayRuntime.cpp").read_text(encoding="utf-8")
    refresh = source[source.index("bool RefreshHud ()") : source.index("void RedrawHud ()")]
    assert "overlaycontrol::FollowHudState ();" in refresh
    assert "layer::PrepareLayers" in refresh
    assert "dxgi::planguest::Prepare" in refresh
    assert "settled.keys.clear (), settled.characters.clear ();" in refresh
    assert "HudInput (false), guestChanged" in source


def test_keyboard_ownership_precedes_accelerators_without_global_hooks():
    source = (ROOT / "OverlayInputKeyboard.cpp").read_text(encoding="utf-8")
    routing = (ROOT / "OverlayInput.cpp").read_text(encoding="utf-8")
    assert "SetWindowsHookExW (WH_KEYBOARD" in source
    assert "::GetCurrentThreadId ()" in source
    assert "WH_KEYBOARD_LL" not in source
    assert "IsChild (window, candidate.canvas)" in routing
    assert "target->map.locked" in routing
    assert "keyboard::Stop ();" in routing


def test_roof_furniture_reuses_model_glyph_projection_not_present_layout():
    source = (ROOT / "OverlaySceneScreen.cpp").read_text(encoding="utf-8")
    assert "v.modelAnchored ? 0u : kScreenAnchored" in source
    assert "v.turn ? kAlongDirection : 0u" in source
    assert "v.modelOffset[0]" in source
    collect = (ROOT / "OverlayHudCollect.cpp").read_text(encoding="utf-8")
    assert "roofcanvas::Bind" in collect
