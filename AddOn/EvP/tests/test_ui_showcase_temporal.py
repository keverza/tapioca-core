"""The private showcase groups live-tested temporal widgets and echoes readback."""

import importlib.util
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources" / "PyPackage"))
import evp
from evp import _scanner

SHOWCASE = Path(__file__).resolve().parents[4] / "private" / "Commands" / "UIShowcase" / "command.py"


def test_temporal_controls_scan_as_one_conditional_group():
    result = _scanner.scan_file(str(SHOWCASE), "UIShowcase")
    params = {entry["name"]: entry for entry in result["params"]}
    assert "Temporal" in params["action"]["args"]
    for name, kind in (
        ("date", "DateProbe"),
        ("time", "TimeProbe"),
        ("calendar", "CalendarProbe"),
        ("hour", "Hour"),
        ("civil_date", "Calendar"),
        ("end_hour", "Hour"),
    ):
        assert params[name]["type"] == kind
        assert params[name]["show_when_param"] == "action"
        assert params[name]["show_when_values"] == ["Temporal"]
    assert params["civil_date"]["default"] == "2026-03-21"
    assert params["end_hour"]["maximum"] == 24


def test_temporal_values_are_echoed_without_native_calls(monkeypatch, capsys):
    monkeypatch.setitem(sys.modules, "tapioca", evp)
    spec = importlib.util.spec_from_file_location("ui_showcase_temporal", SHOWCASE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.run(action="Temporal", date=1788244200, time=1790670780, calendar=1789713000, hour=11)
    output = capsys.readouterr().out
    for value in ("1788244200", "1790670780", "1789713000"):
        assert value in output
    assert "hour" in output and "11" in output
    assert "raw DG Int32" in output and "logs/scan.log" in output
