"""Scanner/runtime and native UI seam checks for manual sun-study advice."""

import importlib.util
from pathlib import Path

ROOT = Path(__file__).parents[1]
PACKAGE = ROOT / "Sources" / "PyPackage" / "evp"
ADDON = ROOT / "Sources" / "AddOn"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_enum_advice_is_literal_metadata_and_does_not_select_a_mode(tmp_path):
    scanner = load("preset_scanner", PACKAGE / "_scanner.py")
    folder = tmp_path / "Preset"
    folder.mkdir()
    entry = folder / "command.py"
    entry.write_text(
        'import evp\n@evp.command()\ndef run(preset: evp.Enum("early model", "late model", '
        'advice="sun-study-size") = "early model"):\n    pass\n',
        encoding="utf-8",
    )
    metadata = scanner.scan_file(str(entry), "Preset")
    param = metadata["params"][0]
    assert param["args"] == ["early model", "late model"]
    assert param["default"] == "early model"
    assert param["advice"] == "sun-study-size"
    command = load("preset_command_types", PACKAGE / "command.py")
    annotation = command.Enum("early model", "late model", advice="sun-study-size")
    assert annotation.advice == "sun-study-size"
    assert annotation.choices == param["args"]


def test_native_advice_is_orange_below_picker_and_never_extracts_or_selects():
    advisory = (ADDON / "Palette" / "ParamAdvisory.cpp").read_text(encoding="utf-8")
    assert "SetTextColor (Gfx::Color" in advisory
    assert "220" in advisory and "120" in advisory and "20" in advisory
    assert "SunStudyPresetAdvice" in advisory
    assert "MeshStore::Get ().Current ()" in advisory
    assert "DiligentViewport::Get ().Stats ()" in advisory
    assert "stats.scenePending == 0" in advisory
    assert "!geomsrv::archviz::ExtractionWorker::Get ().IsRunning ()" in advisory
    assert "SelectItem" not in advisory and "ExecuteNativeCommand" not in advisory and "ACAPI_" not in advisory
    panel = (ADDON / "Palette" / "ParamPanel.cpp").read_text(encoding="utf-8")
    row = panel.split("const auto placeRow", 1)[1].split("const auto placeGroup", 1)[0]
    assert row.index("y += RowHeight + RowGap") < row.index("pc.advisory->PlaceAt")
    assert "pc.advisory->Hide ()" in panel
    shell = (ADDON / "Palette" / "ControlPalette.cpp").read_text(encoding="utf-8")
    idle = shell.split("void ControlPalette::PanelIdle", 1)[1].split("void ControlPalette::RefreshSearchFilter", 1)[0]
    assert "params.RefreshAdvisories ()" in idle
    assert idle.index("itemsDisabled.load ()") < idle.index("params.RefreshAdvisories ()")


def test_native_hud_maps_both_presets_and_follower_keeps_the_selection():
    hud = (ADDON / "ArchViz" / "DiligentHudSunRun.cpp").read_text(encoding="utf-8")
    state = (ADDON / "ArchViz" / "DiligentHud.hpp").read_text(encoding="utf-8")
    request = (ADDON / "NativeCommands" / "SunStudyHudRunner.hpp").read_text(encoding="utf-8")
    runner = (ADDON / "NativeCommands" / "SunStudyHudRunner.cpp").read_text(encoding="utf-8")
    follower = (ADDON / "NativeCommands" / "SunStudyFollowerDriver.cpp").read_text(encoding="utf-8")
    display = (ADDON / "NativeCommands" / "SunStudyDisplayCommands.cpp").read_text(encoding="utf-8")
    commands = (ADDON / "NativeCommands" / "SunStudyCommands.cpp").read_text(encoding="utf-8")

    assert "int sunRunPreset = 0" in state
    assert 'ImGui::RadioButton ("Early model##sunrun", &state.sunRunPreset, 0)' in hud
    assert 'ImGui::RadioButton ("Late model##sunrun", &state.sunRunPreset, 1)' in hud
    assert 'request.preset = state.sunRunPreset == 1 ? "late" : "early"' in hud
    assert 'std::string preset = "early"' in request
    assert "double glassThreshold = 0.4" in request
    assert 'params.Add ("preset", GS::UniString (request.preset.c_str (), CC_UTF8))' in runner
    assert 'params.Add ("glassThreshold", request.glassThreshold)' in runner
    assert 'config.preset = metadata.preset' in display
    assert 'config.glassThreshold = metadata.glassThreshold' in display
    assert 'params.Add ("preset", GS::UniString (config.preset.c_str (), CC_UTF8))' in follower
    assert 'params.Add ("glassThreshold", config.glassThreshold)' in follower
    assert 'if (!start.ok)\n        return failed (start);' in runner
    assert 'status.stage == "failed"' in hud and 'status.error' in hud
    assert "no transparency-qualified glass faces" in commands
    assert 'SunStudyPresetAdvice (' in hud
    assert 'snapshot->captureStamp == captureStamp && snapshot->completeModel' in hud
