"""Diagnostic command orders source creation/read before static dimensions."""

import importlib.util
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
import evp


def _probe(monkeypatch):
    monkeypatch.setitem(sys.modules, "tapioca", evp)
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
    path = os.path.join(root, "private", "Commands", "DraftingRoundTripProbe", "command.py")
    spec = importlib.util.spec_from_file_location("drafting_round_trip_probe", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_creates_sources_then_dimensions_from_readback(monkeypatch):
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

    def create_polyline(points):
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
            record.update(beg_coordinate=(item["x"], item["y"]),
                          end_coordinate=(item["end_x"], item["end_y"]))
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
            record.update(memo_read=True, points=item["points"], line=item["line"],
                          direction=item["direction"])
        elif kind == "radial":
            record.update(base=item["base"], end=item["end"], radius=item["radius"])
        else:
            record.update(origin=item["origin"], bases=[item["origin"], item["ray1"],
                                                           item["origin"], item["ray2"]])
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
    assert raw["angular"]["origin"] == (14.0, 23.0)
    assert any("created=9/9 read=9/9 failures=none" in line for line in lines)


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
    monkeypatch.setattr(evp.drafting, "create_polyline", lambda points: {"ok": False})
    monkeypatch.setattr(evp.paths, "append_log", lambda name, body: lines.extend(body) or "probe.log")
    probe.run(confirm=True, fill="Solid Fill", x=0.0, y=0.0)

    assert attempts == ["line", "arc", "circle", "hotspot"]
    assert any("APIERR_IRREGULARPOLY" in line and "Traceback" in line for line in lines)
    assert any("Cannot draw on line" in line for line in lines)
    assert any("created=0/9 read=0/0" in line for line in lines)
