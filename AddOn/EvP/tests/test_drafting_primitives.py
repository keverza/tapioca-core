"""First native drafting CRUD slice: strict schemas and Python wire contracts."""
import json
import os
import re
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import drafting, elements


_NATIVE = os.path.join(os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands")


def _schemas(filename):
    with open(os.path.join(_NATIVE, filename), encoding="utf-8") as source:
        return [json.loads(blob) for blob in re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_primitive_schemas_are_strict_and_registered():
    create_input, create_output = _schemas("DraftingPrimitiveCommands.cpp")
    assert create_input["additionalProperties"] is False
    assert create_input["properties"]["elements"]["items"]["additionalProperties"] is False
    assert set(create_input["properties"]["elements"]["items"]["properties"]["kind"]["enum"]) == {
        "line", "arc", "circle", "hotspot"
    }
    assert create_output["additionalProperties"] is False
    assert "ok" not in create_output["properties"]

    get_output = _schemas("ElementReadCommands.cpp")[3]["oneOf"][0]
    read_fields = get_output["properties"]["detailsOfElements"]["items"]["properties"]
    assert {"line", "arc", "circle", "hotspot"}.issubset(read_fields["kind"]["enum"])
    assert {"x", "y", "radius", "begAngle", "endAngle"}.issubset(read_fields["details"]["properties"])

    set_input, set_output = _schemas("ElementModifyCommands.cpp")
    writable = set_input["properties"]["edits"]["items"]["properties"]["details"]["properties"]
    assert {"begCoordinate", "endCoordinate", "radius", "pen"}.issubset(writable)
    assert set_output["oneOf"][0]["properties"]["results"]["items"]["properties"]["kind"]["enum"][-4:] == [
        "line", "arc", "circle", "hotspot"
    ]


def test_create_primitives_wire_and_results(monkeypatch):
    captured = {}

    def fake_call(command, params):
        captured.update(command=command, params=params)
        return SimpleNamespace(data={"results": [{
            "kind": "line", "succeeded": True, "elementId": {"guid": "line-1"},
            "databaseId": {"guid": "worksheet-1"}, "layer": "Drafting", "verified": True,
        }]})

    monkeypatch.setattr(drafting, "call", fake_call)
    created = drafting.create_primitives(
        {"kind": "line", "x": 0, "y": 1, "end_x": 2, "end_y": 3, "layer": "Drafting"},
        database_anchor="worksheet-1", fail_on_error=True,
    )
    assert captured["command"] == "Tapioca.CreateDraftingPrimitives"
    assert captured["params"]["elements"][0]["endX"] == 2
    assert captured["params"]["databaseAnchorElementId"] == {"guid": "worksheet-1"}
    assert captured["params"]["failOnError"] is True
    assert created[0] == {"kind": "line", "ok": True, "guid": "line-1", "database_guid": "worksheet-1",
                          "layer": "Drafting", "verified": True, "error": ""}


def test_create_primitives_rejects_wrong_kind_fields_before_write():
    with pytest.raises(ValueError, match="unexpected circle field"):
        drafting.create_primitives({"kind": "circle", "x": 0, "y": 0, "radius": 1, "end_x": 2})
    with pytest.raises(ValueError, match="begAngle"):
        drafting.create_primitives({"kind": "arc", "x": 0, "y": 0, "radius": 1})


def test_create_primitives_batches_arc_circle_hotspot_in_transaction():
    class Tx:
        def call(self, command, params):
            assert command == "Tapioca.CreateDraftingPrimitives"
            assert [item["kind"] for item in params["elements"]] == ["arc", "circle", "hotspot"]
            assert params["elements"][0]["begAngle"] == 0
            assert params["elements"][0]["endAngle"] == 1.5
            return "handle"

    assert drafting.create_primitives([
        {"kind": "arc", "x": 0, "y": 0, "radius": 2, "beg_angle": 0, "end_angle": 1.5},
        {"kind": "circle", "x": 2, "y": 0, "radius": 1},
        {"kind": "hotspot", "x": 4, "y": 0, "height": 0},
    ], tx=Tx()) == "handle"


def test_set_details_uses_typed_id_and_nested_coordinate(monkeypatch):
    captured = {}

    def fake_call(command, params):
        captured.update(command=command, params=params)
        return SimpleNamespace(data={"results": [{"elementId": {"guid": "line-1"}, "kind": "line",
                                                   "succeeded": True, "applied": ["begCoordinate"]}]})

    monkeypatch.setattr(elements, "call", fake_call)
    result = elements.set_details({"line-1": {"beg_coordinate": (2, 3)}})
    assert captured["params"]["edits"] == [{"elementId": {"guid": "line-1"},
                                               "details": {"begCoordinate": {"x": 2, "y": 3, "z": 0.0}}}]
    assert result[0]["guid"] == "line-1" and result[0]["ok"] is True


def test_read_details_normalizes_drafting_geometry(monkeypatch):
    responses = [
        {"elementId": {"guid": "line-1"}, "found": True, "kind": "line", "details": {
            "begCoordinate": {"x": 1, "y": 2, "z": 0}, "endCoordinate": {"x": 3, "y": 4, "z": 0}, "pen": 7}},
        {"elementId": {"guid": "circle-1"}, "found": True, "kind": "circle", "details": {
            "x": 5, "y": 6, "radius": 2.5, "ratio": 1, "angle": 0, "pen": 9}},
        {"elementId": {"guid": "hotspot-1"}, "found": True, "kind": "hotspot", "details": {
            "x": 7, "y": 8, "height": 0, "pen": 1}},
    ]
    monkeypatch.setattr(elements, "call", lambda *_: SimpleNamespace(data={"detailsOfElements": responses}))
    line, circle, hotspot = elements.details(["line-1", "circle-1", "hotspot-1"])
    assert (line["beg_coordinate"], line["end_coordinate"], line["pen"]) == ((1, 2), (3, 4), 7)
    assert (circle["x"], circle["radius"], circle["pen"]) == (5, 2.5, 9)
    assert hotspot["x"] == 7 and hotspot["height"] == 0
