"""Fill-element creation and style round-trip contracts (not fill attributes)."""
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


def test_fill_schemas_and_native_memo_contract():
    request, response = _schemas("DraftingFillCommands.cpp")
    item = request["properties"]["fills"]["items"]
    assert request["additionalProperties"] is False
    assert item["required"] == ["fill", "polygonOutline"]
    assert item["additionalProperties"] is False
    assert item["properties"]["polygonOutline"]["minItems"] == 3
    assert response["additionalProperties"] is False
    assert "ok" not in response["properties"]

    with open(os.path.join(_NATIVE, "DraftingFillCommands.cpp"), encoding="utf-8") as source:
        cpp = source.read()
    assert "element.header.type = API_HatchID" in cpp
    assert "AttributeNameToIndex (API_FilltypeID" in cpp
    assert "ACAPI_Element_Create (&element, &memo)" in cpp
    assert "ACAPI_DisposeElemMemoHdls (&memo)" in cpp

    details_response = _schemas("ElementReadCommands.cpp")[3]["oneOf"][0]
    fields = details_response["properties"]["detailsOfElements"]["items"]["properties"]["details"]["properties"]
    assert fields["fill"] == {"type": "string"}
    edit_request = _schemas("ElementModifyCommands.cpp")[0]
    writable = edit_request["properties"]["edits"]["items"]["properties"]["details"]["properties"]
    assert {"fill", "fillPen", "fillBGPen", "pen"}.issubset(writable)
    assert writable["pen"]["minimum"] == 0


def test_create_fills_sends_typed_wire_and_normalizes_result(monkeypatch):
    captured = {}

    def fake_call(command, params):
        captured.update(command=command, params=params)
        return SimpleNamespace(data={"results": [{
            "succeeded": True, "elementId": {"guid": "fill-1"},
            "databaseId": {"guid": "worksheet-1"}, "layer": "Drafting", "verified": True,
        }]})

    monkeypatch.setattr(drafting, "call", fake_call)
    placed = drafting.create_fills({
        "fill": "Solid Fill", "polygon_outline": [(0, 0), (2, 0), (0, 2)],
        "layer": "Drafting", "pen": 0, "fill_pen": 7, "fill_bg_pen": 0,
    }, database_anchor="worksheet-1", fail_on_error=True)
    assert captured["command"] == "Tapioca.CreateFills"
    assert captured["params"]["fills"][0]["polygonOutline"] == [
        {"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 0.0}, {"x": 0.0, "y": 2.0}]
    assert captured["params"]["fills"][0]["fillBGPen"] == 0
    assert captured["params"]["databaseAnchorElementId"] == {"guid": "worksheet-1"}
    assert captured["params"]["failOnError"] is True
    assert placed == [{"ok": True, "guid": "fill-1", "database_guid": "worksheet-1",
                       "layer": "Drafting", "verified": True, "error": ""}]


def test_create_fills_rejects_unsupported_geometry_and_supports_transaction():
    with pytest.raises(ValueError, match="unknown fill field"):
        drafting.create_fills({"fill": "Solid", "polygon_outline": [(0, 0)] * 3,
                               "holes": [[(1, 1), (2, 1), (2, 2)]]})

    class Tx:
        def call(self, command, params):
            assert command == "Tapioca.CreateFills"
            assert len(params["fills"]) == 2
            return "handle"

    item = {"fill": "Solid", "polygon_outline": [(0, 0), (1, 0), (0, 1)]}
    assert drafting.create_fills([item, item], tx=Tx()) == "handle"


def test_fill_read_and_style_edit_use_same_field_names(monkeypatch):
    response = {"detailsOfElements": [{
        "elementId": {"guid": "fill-1"}, "found": True, "kind": "fill", "details": {
            "fill": "Solid Fill", "pen": 0, "fillPen": 7, "fillBGPen": 0,
            "polygonOutline": [{"x": 0, "y": 0}, {"x": 2, "y": 0}, {"x": 0, "y": 2}],
            "polygonArcs": [0, 0, 0], "holes": [], "hasHoles": False,
        },
    }]}
    monkeypatch.setattr(elements, "call", lambda *_: SimpleNamespace(data=response))
    record = elements.details(["fill-1"])[0]
    assert record["fill"] == "Solid Fill" and record["pen"] == 0
    assert record["footprint"] == [(0, 0), (2, 0), (0, 2)]

    sent = {}

    class Tx:
        def call(self, command, params):
            sent.update(command=command, params=params)
            return "handle"

    assert elements.set_details({"fill-1": {"fill": "Solid Fill", "fill_bg_pen": 0}}, tx=Tx()) == "handle"
    assert sent["params"]["edits"] == [{"elementId": {"guid": "fill-1"},
                                        "details": {"fill": "Solid Fill", "fillBGPen": 0}}]
