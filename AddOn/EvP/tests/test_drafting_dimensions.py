"""Dimension creation, typed reads and sparse style-edit wire contracts."""

import json
import os
import re
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import drafting

_NATIVE = os.path.join(os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands")


def _schemas():
    with open(os.path.join(_NATIVE, "DraftingDimensionCommands.cpp"), encoding="utf-8") as source:
        return [json.loads(blob) for blob in re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_dimension_schemas_and_native_memo_contract():
    create, created, read, response, edit, edited = _schemas()
    assert create["additionalProperties"] is False
    assert create["properties"]["dimensions"]["items"]["additionalProperties"] is False
    assert create["properties"]["dimensions"]["items"]["properties"]["kind"]["enum"] == ["linear", "radial", "angular"]
    assert create["properties"]["dimensions"]["items"]["properties"]["sourceElementId"] == {"$ref": "#ElementId"}
    assert response["properties"]["dimensions"]["items"]["properties"]["sourceElementId"] == {"$ref": "#ElementId"}
    assert created["additionalProperties"] is False
    assert "ok" not in created["properties"]
    assert "level" in response["properties"]["dimensions"]["items"]["properties"]["kind"]["enum"]
    assert read["additionalProperties"] is False
    assert edit["properties"]["failOnError"] == {"type": "boolean"}
    assert edited["additionalProperties"] is False

    with open(os.path.join(_NATIVE, "DraftingDimensionCommands.cpp"), encoding="utf-8") as source:
        cpp = source.read()
    assert "memo.dimElems = reinterpret_cast<API_DimElem**>" in cpp
    assert "ACAPI_Element_Create (&element, typeId == API_DimensionID ? &memo : nullptr)" in cpp
    assert "ACAPI_DisposeElemMemoHdls (&memo)" in cpp
    assert "ACAPI_Element_GetMemo (guid, &memo)" in cpp
    assert "ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, smallArc)" in cpp
    assert "element.radialDimension.base = {};" in cpp
    assert "element.radialDimension.onlyDimensionText = false;" in cpp
    assert "element.radialDimension.textWay = APIDir_Radial;" in cpp
    assert "element.radialDimension.base.base.guid = source.header.guid;" in cpp
    assert "element.radialDimension.base.base.type = source.header.type;" in cpp
    assert "element.radialDimension.base.base.line = true;" in cpp
    assert "radial source must belong to the target database" in cpp
    assert "radial radius and base point must match the source curve" in cpp
    assert "unassociated creation is unsupported" in cpp


def test_create_dimensions_batched_and_anchored(monkeypatch):
    captured = {}

    def fake_call(command, params):
        captured.update(command=command, params=params)
        return SimpleNamespace(
            data={
                "results": [
                    {
                        "kind": "linear",
                        "succeeded": True,
                        "elementId": {"guid": "linear-1"},
                        "databaseId": {"guid": "ws-1"},
                        "layer": "Dimensions",
                        "verified": True,
                    },
                    {"kind": "radial", "succeeded": False, "error": "invalid radius"},
                ]
            }
        )

    monkeypatch.setattr(drafting, "call", fake_call)
    result = drafting.create_dimensions(
        [
            {"kind": "linear", "line": (0, 1), "direction": (1, 0), "points": [(0, 0), (2, 0)], "floor_ind": 1},
            {"kind": "radial", "base": (4, 0), "end": (6, 0), "radius": 4, "source_guid": "circle-1"},
        ],
        database_anchor="ws-1",
        fail_on_error=True,
    )
    assert captured["command"] == "Tapioca.CreateDraftingDimensions"
    assert captured["params"]["dimensions"][0]["points"] == [{"x": 0.0, "y": 0.0}, {"x": 2.0, "y": 0.0}]
    assert captured["params"]["dimensions"][0]["floorInd"] == 1
    assert captured["params"]["databaseAnchorElementId"] == {"guid": "ws-1"}
    assert captured["params"]["failOnError"] is True
    assert result[0]["guid"] == "linear-1" and result[0]["verified"] is True
    assert result[1]["ok"] is False and result[1]["error"] == "invalid radius"


def test_angular_dimension_transaction_and_kind_specific_validation():
    class Tx:
        def call(self, command, params):
            assert command == "Tapioca.CreateDraftingDimensions"
            assert params["dimensions"][0]["smallArc"] is False
            return "handle"

    angular = {"kind": "angular", "origin": (0, 0), "ray1": (1, 0), "ray2": (0, 1), "radius": 2, "small_arc": False}
    assert drafting.create_dimensions(angular, tx=Tx()) == "handle"
    with pytest.raises(ValueError, match="unknown"):
        drafting.create_dimensions(dict(angular, points=[(0, 0), (1, 0)]), tx=Tx())


def test_associated_radial_source_wire_and_readback(monkeypatch):
    captured = {}

    class Tx:
        def call(self, command, params):
            captured.update(command=command, params=params)
            return "handle"

    radial = {"kind": "radial", "base": (4, 0), "end": (6, 0), "radius": 4, "source_guid": "circle-1"}
    assert drafting.create_dimensions(radial, tx=Tx()) == "handle"
    assert captured["params"]["dimensions"][0]["sourceElementId"] == {"guid": "circle-1"}
    with pytest.raises(ValueError, match="non-empty"):
        drafting.create_dimensions(dict(radial, source_guid=""), tx=Tx())
    with pytest.raises(ValueError, match="unknown"):
        drafting.create_dimensions(
            {
                "kind": "linear",
                "line": (0, 1),
                "direction": (1, 0),
                "points": [(0, 0), (2, 0)],
                "source_guid": "circle-1",
            },
            tx=Tx(),
        )

    monkeypatch.setattr(
        drafting,
        "call",
        lambda *_: SimpleNamespace(
            data={
                "dimensions": [
                    {
                        "kind": "radial",
                        "elementId": {"guid": "radial-1"},
                        "sourceElementId": {"guid": "circle-1"},
                        "base": {"x": 4, "y": 0},
                        "end": {"x": 6, "y": 0},
                        "radius": 4,
                    }
                ]
            }
        ),
    )
    record = drafting.dimensions(["radial-1"])[0]
    assert record["source_guid"] == "circle-1"
    assert "sourceElementId" not in record
    assert record["radius"] == 4


@pytest.mark.parametrize("source_guid", [None, "", "   "])
def test_radial_rejects_empty_source_before_dispatch(monkeypatch, source_guid):
    def unexpected_call(*args):
        pytest.fail("invalid radial source reached native dispatch")

    monkeypatch.setattr(drafting, "call", unexpected_call)
    radial = {"kind": "radial", "base": (1, 0), "end": (2, 0), "radius": 1}
    with pytest.raises(ValueError, match="source_guid"):
        drafting.create_dimensions(radial)
    with pytest.raises(ValueError, match="source_guid"):
        drafting.create_dimensions(dict(radial, source_guid=source_guid))


@pytest.mark.parametrize("point", [None, (1,), (1, 2, 3), (float("nan"), 0), (0, float("inf")), ("invalid", 0)])
def test_dimension_rejects_invalid_point_before_dispatch(monkeypatch, point):
    monkeypatch.setattr(drafting, "call", lambda *args: pytest.fail("invalid point reached native dispatch"))
    with pytest.raises(ValueError, match="finite|missing"):
        drafting.create_dimensions({"kind": "linear", "line": point, "direction": (1, 0), "points": [(0, 0), (1, 0)]})


def test_read_dimensions_and_sparse_style_edit(monkeypatch):
    captured = {}

    def fake_call(command, params):
        captured.update(command=command, params=params)
        return SimpleNamespace(
            data={
                "dimensions": [
                    {
                        "elementId": {"guid": "linear-1"},
                        "kind": "linear",
                        "floorInd": 0,
                        "line": {"x": 0, "y": 1},
                        "points": [{"x": 0, "y": 0}, {"x": 2, "y": 0}],
                        "memoRead": True,
                        "nDimElem": 2,
                    }
                ]
            }
        )

    monkeypatch.setattr(drafting, "call", fake_call)
    records = drafting.dimensions(["linear-1"])
    assert captured == {
        "command": "Tapioca.GetDraftingDimensions",
        "params": {"scope": "database", "elements": [{"elementId": {"guid": "linear-1"}}]},
    }
    assert records[0]["points"] == [(0, 0), (2, 0)]
    assert records[0]["memo_read"] is True and records[0]["n_dim_elem"] == 2

    class Tx:
        def call(self, command, params):
            captured.update(command=command, params=params)
            return "handle"

    assert (
        drafting.set_dimension_style(
            {"linear-1": {"pen": 7}, "angle-1": {"small_arc": False}}, fail_on_error=True, tx=Tx()
        )
        == "handle"
    )
    assert captured["command"] == "Tapioca.SetDraftingDimensionStyle"
    assert captured["params"]["edits"][1] == {"elementId": {"guid": "angle-1"}, "smallArc": False}
    assert captured["params"]["failOnError"] is True
    with pytest.raises(ValueError, match="unsupported"):
        drafting.set_dimension_style({"linear-1": {"points": [(0, 0), (1, 0)]}})
