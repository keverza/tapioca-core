"""Diagnostic command orders source creation/read before static dimensions."""

import importlib.util
import math
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
import evp
from evp import _scanner


def _probe(monkeypatch):
    monkeypatch.setitem(sys.modules, "tapioca", evp)
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
    path = os.path.join(root, "private", "Commands", "DraftingRoundTripProbe", "command.py")
    spec = importlib.util.spec_from_file_location("drafting_round_trip_probe", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("extend_radial_leader", [False, True])
def test_creates_sources_then_dimensions_from_readback(monkeypatch, extend_radial_leader):
    probe = _probe(monkeypatch)
    history = []
    raw = {}
    lines = []

    def create_primitives(item):
        kind = item["kind"]
        history.append("create:" + kind)
        raw[kind] = item
        return [{"ok": True, "guid": kind, "verified": True}]

    def create_fills(item):
        history.append("create:fill")
        raw["fill"] = item
        return [{"ok": True, "guid": "fill", "verified": True}]

    def create_polyline(points, layer=None):
        history.append("create:polyline")
        raw["polyline"] = points
        return {"ok": True, "guid": "polyline", "verified": True}

    def create_dimensions(item):
        kind = item["kind"]
        history.append("create:" + kind)
        raw[kind] = item
        return [{"ok": True, "guid": kind, "verified": True}]

    def details(guids):
        kind = guids[0]
        history.append("read:" + kind)
        item = raw[kind]
        record = {"guid": kind, "kind": kind, "found": True}
        if kind == "line":
            record.update(beg_coordinate=(item["x"], item["y"]), end_coordinate=(item["end_x"], item["end_y"]))
        elif kind in ("arc", "circle", "hotspot"):
            record.update(x=item["x"], y=item["y"], radius=item.get("radius"))
            if kind == "arc":
                record.update(beg_angle=item["beg_angle"], end_angle=item["end_angle"])
        elif kind == "fill":
            record.update(footprint=item["polygon_outline"], fill=item["fill"])
        else:
            record["footprint"] = item
        return [record]

    def dimensions(guids):
        kind = guids[0]
        history.append("read:" + kind)
        item = raw[kind]
        record = {"guid": kind, "kind": kind}
        if kind == "linear":
            record.update(memo_read=True, points=item["points"], line=item["line"], direction=item["direction"])
        elif kind == "radial":
            record.update(base=item["base"], end=item["end"], radius=item["radius"], source_guid=item["source_guid"])
            if extend_radial_leader:
                record["end"] = (item["end"][0] + 0.2, item["end"][1])
        else:
            record.update(origin=item["origin"], bases=[item["origin"], item["ray1"], item["origin"], item["ray2"]])
        return [record]

    monkeypatch.setattr(evp.drafting, "create_primitives", create_primitives)
    monkeypatch.setattr(evp.drafting, "create_fills", create_fills)
    monkeypatch.setattr(evp.drafting, "create_polyline", create_polyline)
    monkeypatch.setattr(evp.drafting, "create_dimensions", create_dimensions)
    monkeypatch.setattr(evp.drafting, "dimensions", dimensions)
    monkeypatch.setattr(evp.elements, "details", details)
    monkeypatch.setattr(evp.paths, "append_log", lambda name, body: lines.extend(body) or "probe.log")

    probe.run(confirm=True, fill="Solid Fill", x=10.0, y=20.0)

    assert len([event for event in history if event.startswith("create:")]) == 9
    assert history.index("read:polyline") < history.index("create:linear")
    assert raw["linear"]["points"] == [(10.0, 20.0), (11.0, 20.0)]
    assert raw["radial"]["base"] == (15.5, 20.0)
    assert raw["radial"]["radius"] == 0.5
    assert raw["radial"]["source_guid"] == "circle"
    assert raw["angular"]["origin"] == (14.0, 23.0)
    assert raw["fill"]["layer"] == "2D Drafting - General"
    assert any("created=9/9 read=9/9 failures=none" in line for line in lines)
    assert any("radial leader adjusted" in line for line in lines) is extend_radial_leader


def test_one_failed_create_does_not_hide_other_errors(monkeypatch):
    probe = _probe(monkeypatch)
    attempts = []
    lines = []

    def failing_primitive(item):
        attempts.append(item["kind"])
        if item["kind"] == "arc":
            raise RuntimeError("APIERR_IRREGULARPOLY")
        return [{"ok": False, "error": "APIERR_BADPARS"}]

    monkeypatch.setattr(evp.drafting, "create_primitives", failing_primitive)
    monkeypatch.setattr(evp.drafting, "create_fills", lambda item: [{"ok": False, "error": "bad fill"}])
    monkeypatch.setattr(evp.drafting, "create_polyline", lambda points, layer=None: {"ok": False})
    monkeypatch.setattr(evp.paths, "append_log", lambda name, body: lines.extend(body) or "probe.log")
    probe.run(confirm=True, fill="Solid Fill", x=0.0, y=0.0)

    assert attempts == ["line", "arc", "circle", "hotspot"]
    assert any("APIERR_IRREGULARPOLY" in line and "Traceback" in line for line in lines)
    assert any("Cannot draw on line" in line for line in lines)
    assert any("created=0/9 read=0/0" in line for line in lines)


def test_combined_probe_scans_native_date_time_controls(monkeypatch):
    probe = _probe(monkeypatch)
    result = _scanner.scan_file(probe.__file__, "DraftingRoundTripProbe")
    params = {entry["name"]: entry for entry in result["params"]}
    for name, kind in (("date", "DateProbe"), ("time", "TimeProbe"), ("calendar", "CalendarProbe")):
        assert params[name]["type"] == kind
        assert params[name]["default"] == 0


def test_date_time_readback_runs_without_drafting_confirmation(monkeypatch):
    probe = _probe(monkeypatch)
    lines = []

    def forbidden_create(*args, **kwargs):
        raise AssertionError("Unconfirmed probe must not create drafting elements")

    for name in ("create_primitives", "create_fills", "create_polyline", "create_dimensions"):
        monkeypatch.setattr(evp.drafting, name, forbidden_create)
    monkeypatch.setattr(evp.paths, "append_log", lambda name, body: lines.extend(body) or "probe.log")

    probe.run(confirm=False, date=1790670600, time=34200, calendar=1790636400)

    for control, value in (("DateControl", 1790670600), ("TimeControl", 34200), ("CalendarControl", 1790636400)):
        assert any(control in line and "raw Int32=%d" % value in line for line in lines)
    assert any("logs/scan.log" in line for line in lines)
    assert any("created=0/9 read=0/0 failures=none" in line for line in lines)


def test_date_time_readback_runs_when_drafting_input_is_missing(monkeypatch):
    probe = _probe(monkeypatch)
    lines = []
    monkeypatch.setattr(evp.paths, "append_log", lambda name, body: lines.extend(body) or "probe.log")

    probe.run(confirm=True, fill="", date=123, time=456, calendar=789)

    assert any("TimeControl" in line and "raw Int32=456" in line for line in lines)
    assert any("fill input" in line and "FAIL" in line for line in lines)


def test_radial_readback_detects_wrong_source(monkeypatch):
    probe = _probe(monkeypatch)
    expected = {"kind": "radial", "base": (1, 0), "end": (2, 0), "radius": 1, "source_guid": "circle-1"}
    actual = dict(expected, source_guid="circle-2")
    assert "radial source" in probe._check_geometry("radial", actual, expected)


@pytest.mark.parametrize(
    "end, matches",
    [
        ((105.9, 100), True),
        ((106.1, 100), True),
        ((105.8, 100), False),
        ((105.1, 100), False),
        ((105.5, 100), False),
        ((106.1, 100.01), False),
        ((float("nan"), 100), False),
        ((float("inf"), 100), False),
        (None, False),
        ((106.1,), False),
    ],
)
def test_radial_leader_readback_allows_only_same_ray_extension(monkeypatch, end, matches):
    probe = _probe(monkeypatch)
    expected = {"kind": "radial", "base": (105.5, 100), "end": (105.9, 100), "radius": 0.5, "source_guid": "circle-1"}
    actual = dict(expected, end=end)
    mismatch = probe._check_geometry("radial", actual, expected)
    assert (not mismatch) is matches


def test_radial_leader_normalization_preserves_base_radius_and_source_checks(monkeypatch):
    probe = _probe(monkeypatch)
    expected = {"kind": "radial", "base": (1, 1), "end": (2, 2), "radius": 1, "source_guid": "circle-1"}
    actual = dict(expected, end=(3, 3))
    assert probe._check_geometry("radial", actual, expected) == ""
    for key, wrong in (("base", (1, 1.01)), ("radius", 2), ("source_guid", "circle-2")):
        assert probe._check_geometry("radial", dict(actual, **{key: wrong}), expected)


@pytest.mark.parametrize("arc_read_ok", [True, False])
def test_radial_uses_only_verified_arc_when_circle_fails(monkeypatch, arc_read_ok):
    probe = _probe(monkeypatch)
    lines = []
    radial_requests = []

    def create_primitives(item):
        if item["kind"] == "arc":
            return [{"ok": True, "guid": "arc", "verified": True}]
        return [{"ok": False, "error": "wrong type"}]

    def details(guids):
        return [
            {
                "guid": "arc",
                "found": arc_read_ok,
                "kind": "arc",
                "x": 13.0,
                "y": 20.0,
                "radius": 0.5,
                "beg_angle": 0.0,
                "end_angle": math.pi / 2.0,
            }
        ]

    def create_dimensions(item):
        radial_requests.append(item)
        return [{"ok": True, "guid": "radial", "verified": True}]

    def dimensions(guids):
        return [dict(radial_requests[0], guid="radial")]

    monkeypatch.setattr(evp.drafting, "create_primitives", create_primitives)
    monkeypatch.setattr(evp.drafting, "create_fills", lambda item: [{"ok": False}])
    monkeypatch.setattr(evp.drafting, "create_polyline", lambda *args, **kwargs: {"ok": False})
    monkeypatch.setattr(evp.elements, "details", details)
    monkeypatch.setattr(evp.drafting, "create_dimensions", create_dimensions)
    monkeypatch.setattr(evp.drafting, "dimensions", dimensions)
    monkeypatch.setattr(evp.paths, "append_log", lambda name, body: lines.extend(body) or "probe.log")

    probe.run(confirm=True, fill="Solid Fill", x=10.0, y=20.0)

    assert any("circle create" in line and "FAIL" in line for line in lines)
    if arc_read_ok:
        assert len(radial_requests) == 1
        radial = radial_requests[0]
        assert radial["kind"] == "radial"
        assert radial["source_guid"] == "arc"
        assert radial["base"] == pytest.approx((13.0 + 0.5 / math.sqrt(2), 20.0 + 0.5 / math.sqrt(2)))
        assert radial["end"] == pytest.approx((13.0 + 0.9 / math.sqrt(2), 20.0 + 0.9 / math.sqrt(2)))
        assert any("radial round-trip" in line and "PASS" in line for line in lines)
        assert any("Circle failed; using" in line for line in lines)
    else:
        assert not radial_requests
        assert any("Cannot draw on circle" in line for line in lines)
