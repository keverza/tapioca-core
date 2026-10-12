"""Polyline baking stays backwards compatible and targets a story explicitly."""

import json
import re
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources" / "PyPackage"))
from evp import drafting


def test_polyline_schema_has_strict_optional_floor_plan_story_and_pen():
    source = (
        Path(__file__).resolve().parents[1] / "Sources" / "AddOn" / "NativeCommands" / "DraftingPolylineCommands.cpp"
    )
    schemas = [json.loads(blob) for blob in re.findall(r'R"json\((.*?)\)json"', source.read_text(), re.DOTALL)]
    create_input, create_output = schemas[2:]
    assert create_input["additionalProperties"] is False
    assert create_input["required"] == ["coordinates"]
    properties = create_input["properties"]
    assert properties["floorPlan"] == {"type": "boolean"}
    assert properties["floorInd"] == {"type": "integer", "minimum": -32768, "maximum": 32767}
    assert properties["pen"] == {"type": "integer", "minimum": 1, "maximum": 255}
    assert create_output["additionalProperties"] is False


def test_polyline_floor_plan_baking_records_one_transaction_step():
    class Tx:
        def call(self, name, params):
            assert name == "Tapioca.CreateDraftingPolyline"
            assert params == {
                "coordinates": [
                    {"x": 0.0, "y": 0.0},
                    {"x": 2.5, "y": 0.0},
                    {"x": 2.5, "y": 5.0},
                    {"x": 0.0, "y": 5.0},
                    {"x": 0.0, "y": 0.0},
                ],
                "layer": "Parking",
                "floorPlan": True,
                "floorInd": -2,
                "pen": 4,
            }
            return "handle"

    assert (
        drafting.create_polyline(
            [(0, 0), (2.5, 0), (2.5, 5), (0, 5), (0, 0)], layer="Parking", floor_plan=True, floor_ind=-2, pen=4, tx=Tx()
        )
        == "handle"
    )


def test_legacy_polyline_call_omits_new_optional_fields(monkeypatch):
    def fake(name, params):
        assert name == "Tapioca.CreateDraftingPolyline"
        assert params == {"coordinates": [{"x": 0.0, "y": 0.0}, {"x": 1.0, "y": 1.0}], "layer": "Drafting"}
        return SimpleNamespace(data={"elementId": {"guid": "test"}, "verified": True})

    monkeypatch.setattr(drafting, "call", fake)
    assert drafting.create_polyline([(0, 0), (1, 1)], layer="Drafting")["guid"] == "test"


@pytest.mark.parametrize(
    "kwargs",
    [
        {"floor_ind": 32768},
        {"floor_ind": True},
        {"floor_ind": 1.5},
        {"pen": 0},
        {"pen": 256},
        {"pen": False},
        {"pen": 1.5},
        {"floor_plan": True, "database_anchor": "worksheet"},
    ],
)
def test_invalid_bake_settings_fail_before_native_write(kwargs):
    with pytest.raises(ValueError):
        drafting.create_polyline([(0, 0), (1, 1)], **kwargs)


def test_nonfinite_coordinate_fails_before_native_write():
    with pytest.raises(ValueError, match="finite"):
        drafting.create_polyline([(0, 0), (float("inf"), 1)])
