"""Offline bake planning and transport-ledger regressions; never creates elements."""

import copy
import sys
import uuid
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources" / "PyPackage"))
from tapioca.massing.bake import execute, plan


def attribute(name="Chosen"):
    return {"name": name, "guid": str(uuid.uuid4()), "index": 5}


def ring(x=0, size=10):
    return [{"x": x, "y": x}, {"x": x + size, "y": x}, {"x": x + size, "y": x + size}, {"x": x, "y": x + size}]


def request(kind="slices", walls=False, composite=False):
    settings = {
        "layer": attribute("Baked"),
        "material": attribute("Concrete"),
        "composite": attribute("Slab assembly"),
        "surface": attribute("Stone"),
        "compositeOn": composite,
        "slabThickness": 0.25,
        "walls": walls,
        "wallLayer": attribute("Walls"),
        "wallMaterial": attribute("Brick"),
        "wallComposite": attribute("Wall assembly"),
        "wallCompositeOn": composite,
        "wallThickness": 0.3,
        "fill": attribute("Hatch"),
        "line": attribute("Dashed"),
        "pen": 10,
        "fillPen": 20,
    }
    return {
        "kind": kind,
        "token": 1,
        "settings": settings,
        "items": [{"outer": ring(), "holes": [ring(2, 2)], "z": 103.4, "height": 4.2}],
    }


class Bus:
    def __init__(self, fail=None, mixed=False, available=True, expire=False):
        self.calls = []
        self.fail, self.mixed, self.available, self.expire = fail, mixed, available, expire
        self.created = []

    def __call__(self, command, params):
        self.calls.append((command, copy.deepcopy(params)))
        if command == self.fail:
            raise RuntimeError("transport failed")
        if command == "Tapioca.MassingBakeGuard":
            return SimpleNamespace(data={"current": not (self.expire and self.created)})
        if command == "API.IsAddOnCommandAvailable":
            return SimpleNamespace(data={"available": self.available})
        if command == "Tapioca.FinishMassingBake":
            return SimpleNamespace(data={"count": len(params["elements"])})
        field = {
            "Tapioca.BakeMassingSlices": "slabs",
            "Tapir.CreateSlabs": "slabsData",
            "Tapir.CreateWalls": "wallsData",
            "Tapir.CreateMorphs": "morphsData",
            "Tapioca.CreateFills": "fills",
        }[command]
        records = []
        inputs = params[field]
        if command == "Tapioca.BakeMassingSlices":
            inputs = [part for slab in inputs for part in [slab] + ([None] * 8 if "wall" in slab else [])]
        for i, _ in enumerate(inputs):
            if self.mixed and i:
                records.append({"error": "failed element"})
            else:
                guid = str(uuid.uuid4())
                self.created.append(guid)
                records.append({"elementId": {"guid": guid}})
        return SimpleNamespace(
            data={"results" if command == "Tapioca.CreateFills" else "elements": records, "count": len(records)}
        )


def test_slabs_use_native_physical_elevation_construction_and_complete_holes():
    value = request()
    original = copy.deepcopy(value)
    result = plan(value)
    assert value == original
    assert result["primary"][0]["z"] == 103.4
    assert result["primary"][0]["structure"] == "basic"
    assert result["primary"][0]["holes"] == [{"polygonOutline": value["items"][0]["holes"][0]}]
    assert result["wallCount"] == 0


@pytest.mark.parametrize("composite", [False, True])
def test_optional_walls_include_courtyard_and_chosen_construction(composite):
    result = plan(request(walls=True, composite=composite))
    assert result["wallCount"] == 8
    slab = result["primary"][0]
    assert slab["z"] == 103.4
    assert slab["height"] == 4.2
    wall = slab["wall"]
    assert "floorIndex" not in wall  # Home stories are assigned natively from physical building floors.
    assert wall["structure"] == ("composite" if composite else "basic")
    assert wall["attribute"] == ("Wall assembly" if composite else "Brick")
    assert result["wallLayer"] == "Walls"


def test_slabs_and_walls_use_one_atomic_native_write_with_correct_layer_mapping():
    bus = Bus()
    result = execute(request(walls=True), bus)
    assert result["state"] == "complete"
    assert len(result["createdGuids"]) == 9
    writes = [name for name, _ in bus.calls if not name.endswith("Guard") and not name.startswith("API.")]
    assert writes == ["Tapioca.BakeMassingSlices"]
    final = bus.calls[-1][1]["slabs"]
    assert final[0]["layer"] == "Baked"
    assert final[0]["attribute"] == "Concrete"
    assert final[0]["wall"]["layer"] == "Walls"


def test_collapse_retains_holes_fill_line_pens_and_one_native_undo_call():
    bus = Bus()
    result = execute(request("collapse"), bus)
    assert result["state"] == "complete"
    call = bus.calls[-1]
    assert call[0] == "Tapioca.CreateFills"
    assert call[1]["failOnError"]
    assert call[1]["floorPlan"]  # A plan Fill even when Bake was clicked in the 3D HUD.
    fill = call[1]["fills"][0]
    assert fill["holes"] and fill["fill"] == "Hatch" and fill["lineType"] == "Dashed"
    assert fill["fillBGPen"] == 0 and fill["pen"] == 10 and fill["fillPen"] == 20
    assert "floorInd" not in fill


def test_collapse_transports_signed_real_arcs_on_outer_and_holes():
    value = request("collapse")
    value["items"][0]["arcs"] = [0.4, 0, 0, 0]
    value["items"][0]["holeArcs"] = [[-0.3, 0, 0, 0]]
    bus = Bus()
    assert execute(value, bus)["state"] == "complete"
    fill = bus.calls[-1][1]["fills"][0]
    assert fill["arcAngles"] == [0.4, 0, 0, 0]
    assert fill["holes"][0]["arcAngles"] == [-0.3, 0, 0, 0]
    value["items"][0]["arcs"][0] = float("nan")
    bus = Bus()
    assert execute(value, bus)["state"] == "refused"
    assert not bus.calls


def test_physical_building_group_is_preserved_and_native_bake_does_not_assign_source_metadata():
    value = request(walls=True)
    value["items"][0]["group"] = "building:CaseSensitive ID"
    prepared = plan(value)
    assert prepared["primary"][0]["group"] == "building:CaseSensitive ID"
    assert prepared["create"] == "Tapioca.BakeMassingSlices"
    assert "tapioca.role" not in str(prepared)


def test_envelope_uses_arbitrary_body_and_chosen_surface_without_box_fallback():
    value = request("envelope")
    value["items"] = [
        {
            "basePoint": {"x": 0, "y": 0, "z": 103},
            "body": {
                "bodyType": "Solid",
                "vertices": [
                    {"x": 0, "y": 0, "z": 0},
                    {"x": 1, "y": 0, "z": 0},
                    {"x": 0, "y": 1, "z": 0},
                    {"x": 0, "y": 0, "z": 1},
                ],
                "polygons": [{"vertexIds": ids} for ids in ([0, 2, 1], [0, 1, 3], [0, 3, 2], [1, 2, 3])],
            },
        }
    ]
    bus = Bus()
    result = execute(value, bus)
    assert result["state"] == "complete"
    params = next(params for name, params in bus.calls if name == "Tapir.CreateMorphs")
    assert params["morphsData"][0]["body"] == value["items"][0]["body"]
    assert "surfaceId" in params["morphsData"][0]
    assert "size" not in params["morphsData"][0]
    final = bus.calls[-1][1]["elements"][0]
    assert final["morphSurface"] == "Stone"
    assert final["morphMaterial"] == "Concrete"


def test_empty_morph_body_is_refused_before_any_api_call():
    data = request("envelope")
    data["items"] = [
        {"basePoint": {"x": 0, "y": 0, "z": 0}, "body": {"bodyType": "Solid", "vertices": [], "polygons": []}}
    ]
    bus = Bus()
    assert execute(data, bus)["state"] == "refused"
    assert not bus.calls


def test_native_script_runs_through_the_actual_graph_bridge(monkeypatch):
    import tapioca
    from evp._graphscript import run

    bus = Bus()
    monkeypatch.setattr(tapioca.api, "call", bus)
    result = run(
        "from tapioca.massing.bake import execute\npayload = execute(request)\n",
        "<native Massing bake>",
        {"request": request()},
        [{"portId": "payload"}],
        budget_ms=5000,
    )
    assert result["ok"], result["error"]
    assert result["outputs"]["payload"]["state"] == "complete"
    assert result["outputs"]["payload"]["createdGuids"] == bus.created


@pytest.mark.parametrize("command", ["Tapioca.BakeMassingSlices"])
def test_failed_stage_returns_known_created_ledger_and_never_retries(command):
    bus = Bus(fail=command)
    result = execute(request(walls=True), bus)
    assert result["state"] in {"partial", "refused"}
    assert result["createdGuids"] == bus.created
    assert sum(name == command for name, _ in bus.calls) == 1
    assert "No writes retried" in result["note"]


def test_slab_wall_baking_does_not_require_tapir():
    bus = Bus(available=False)
    result = execute(request(), bus)
    assert result["state"] == "complete"
    assert len(bus.created) == 1
    assert not any(name.startswith("Tapir.") or name.startswith("API.") for name, _ in bus.calls)


def test_slabs_and_walls_have_no_followup_write_to_race_project_closure():
    bus = Bus(expire=True)
    result = execute(request(walls=True), bus)
    assert result["state"] == "complete"
    assert len(result["createdGuids"]) == 9
    assert not any(name in {"Tapir.CreateWalls", "Tapioca.FinishMassingBake"} for name, _ in bus.calls)


@pytest.mark.parametrize(
    "key,value", [("slabThickness", float("nan")), ("slabThickness", 0), ("material", {}), ("layer", {})]
)
def test_invalid_settings_are_refused_before_any_api_call(key, value):
    data = request()
    data["settings"][key] = value
    bus = Bus()
    assert execute(data, bus)["state"] == "refused"
    assert not bus.calls


def test_partial_batch_collects_all_successful_ids_even_with_failed_siblings():
    data = request()
    data["items"].append(copy.deepcopy(data["items"][0]))
    bus = Bus(mixed=True)
    result = execute(data, bus)
    assert result["state"] == "partial"
    assert result["createdGuids"] == bus.created
    assert len(result["createdGuids"]) == 1
    assert not any(name == "Tapioca.FinishMassingBake" for name, _ in bus.calls)
