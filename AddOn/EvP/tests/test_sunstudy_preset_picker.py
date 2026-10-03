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
