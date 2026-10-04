"""Civil-date wire contract and end-of-day hours for the native study palette."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[1] / "Sources/PyPackage"))
from evp import Calendar, Hour, _ports, _scanner


def test_calendar_annotation_and_schema_have_the_same_civil_date(tmp_path):
    assert Calendar.kind == "Calendar"
    assert Calendar(readonly=True).readonly
    path = tmp_path / "command.py"
    path.write_text(
        'import tapioca\n@tapioca.command()\ndef run(date: tapioca.Calendar = "2028-02-29"):\n    pass\n',
        encoding="utf-8",
    )
    entry = _scanner.scan_file(str(path), "Calendar")["params"][0]
    schema = {"properties": {"date": {"type": "string", "default": "2028-02-29", "x-port": {"control": "calendar"}}}}
    port = _ports.ports_from_schema(schema)[0]
    assert entry["type"] == port["type"] == "Calendar"
    assert entry["default"] == port["default"] == "2028-02-29"


@pytest.mark.parametrize("value", ["2026-02-29", "2026-3-21", "2038-01-01", "1901-12-31", "20260321", 0])
def test_invalid_calendar_defaults_are_rejected_by_both_routes(tmp_path, value):
    path = tmp_path / "command.py"
    path.write_text(
        "import tapioca\n@tapioca.command()\ndef run(date: tapioca.Calendar = %r):\n    pass\n" % value,
        encoding="utf-8",
    )
    with pytest.raises(_scanner.ScanError, match="Calendar default"):
        _scanner.scan_file(str(path), "Calendar")
    schema = {"properties": {"date": {"type": "string", "default": value, "x-port": {"control": "calendar"}}}}
    with pytest.raises(_ports.PortError, match="Calendar field"):
        _ports.ports_from_schema(schema)


def test_end_of_day_is_explicit_not_a_timezone_epoch(tmp_path):
    assert Hour(maximum=24).maximum == 24
    path = tmp_path / "command.py"
    path.write_text(
        "import tapioca\n@tapioca.command()\ndef run(hour_to: tapioca.Hour(maximum=24) = 24):\n    pass\n",
        encoding="utf-8",
    )
    entry = _scanner.scan_file(str(path), "Hours")["params"][0]
    assert entry["maximum"] == entry["default"] == 24
    schema = {
        "properties": {"hour_to": {"type": "integer", "default": 24, "maximum": 24, "x-port": {"control": "hour"}}}
    }
    assert _ports.ports_from_schema(schema)[0]["maximum"] == 24


@pytest.mark.parametrize("maximum", [1, 25, True, -1])
def test_hour_maximum_is_validated_even_without_a_default(tmp_path, maximum):
    with pytest.raises(ValueError, match="maximum"):
        Hour(maximum=maximum)
    path = tmp_path / "command.py"
    path.write_text(
        "import tapioca\n@tapioca.command()\ndef run(hour: tapioca.Hour(maximum=%r)):\n    pass\n" % maximum,
        encoding="utf-8",
    )
    with pytest.raises(_scanner.ScanError, match="Hour maximum"):
        _scanner.scan_file(str(path), "Hours")


def test_smoke_uses_calendar_and_native_clock_hours():
    path = Path(__file__).parents[4] / "private/Diagnostics/Commands/SunStudyNativeSmoke/command.py"
    if not path.is_file():
        pytest.skip("private diagnostic command absent")
    params = {entry["name"]: entry for entry in _scanner.scan_file(str(path), "Smoke")["params"]}
    assert params["date"]["type"] == "Calendar"
    assert params["hour_from"]["type"] == params["hour_to"]["type"] == "Hour"
    assert params["hour_to"]["maximum"] == 24
    assert "month" not in params and "day" not in params


def test_analysis_controls_and_minimal_tooltip_are_wired():
    root = Path(__file__).parents[1] / "Sources/AddOn"
    shell = (root / "ArchViz/DiligentHudShell.cpp").read_text(encoding="utf-8")
    assert 'tabs.push_back ({ kSunStudyKey, "Analysis" })' in shell
    hud = (root / "ArchViz/DiligentHudSunStudy.cpp").read_text(encoding="utf-8")
    for label in ("Sunstudy", "Shadows", "Roles"):
        assert f'ImGui::Button ("{label}"' in hud
    assert "hide the rest" not in hud
    assert "Diagnostic views" not in hud and "kInspectModes" not in hud
    assert hud.index("DrawSunStudyInspectControl (state.sunInspect") < hud.index("if (shadows)")
    controls = (root / "ArchViz/DiligentHudSunControls.cpp").read_text(encoding="utf-8")
    assert "Off##suninspect" in controls and "Cursor##suninspect" in controls and "Panel##suninspect" in controls
    assert 'ImGui::BeginChild ("##sun-reading"' in controls
    tooltip = hud.split("void DrawSunStudyInspectorTooltip", 1)[1].split("void DrawSunStudyHudSection", 1)[0]
    assert "SunStudyClock (state.sunReadingHours, true)" in tooltip
    assert "ReadingLines" not in tooltip and "scaling corrected" not in tooltip
    assert "ImGuiStyleVar_PopupRounding, 4.0f" in tooltip
    assert "0.78f, 0.78f, 0.78f, 0.70f" in tooltip
    assert "ImGuiStyleVar_WindowPadding, ImVec2 (4.0f, 2.0f)" in tooltip


def test_study_display_blends_over_white_shaded_geometry_without_changing_materials():
    root = Path(__file__).parents[1] / "Sources/AddOn/ArchViz"
    draw = (root / "DiligentSceneDraw.cpp").read_text(encoding="utf-8")
    assert "studyModel ? 1.0f : mat.r" in draw
    assert "studyModel ? 1.0f : mat.alpha" in draw
    assert "const bool blended = !studyModel" in draw
    assert "studyMode == 0 || studyMode >= 5" in draw
    assert "impl_->renderMode =" not in draw
    pipeline = (root / "DiligentSceneSunStudy.cpp").read_text(encoding="utf-8")
    assert "rt.SrcBlend = Diligent::BLEND_FACTOR_SRC_ALPHA" in pipeline
    assert "rt.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA" in pipeline
    shader = (root / "DiligentShaders.hpp").read_text(encoding="utf-8")
    assert "float4 (colour, 0.90)" in shader
    assert "255.0), 0.60)" in shader
