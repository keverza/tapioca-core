"""Shared snapshot solver used by native ImGui, without Archicad or UI calls."""

import copy
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources" / "PyPackage"))
from evp._graphscript import run
from tapioca.massing.native import calculate, preview


def request(concave=False):
    ring = [(0, 0), (30, 0), (30, 12), (12, 12), (12, 30), (0, 30)] if concave else [(0, 0), (20, 0), (20, 20), (0, 20)]
    return {
        "version": 1,
        "edges": [
            {
                "a": a,
                "b": ring[(i + 1) % len(ring)],
                "arc": 0,
                "distance": 1 if concave else 3,
                "regulated": True,
                "reference": True,
                "review": False,
            }
            for i, a in enumerate(ring)
        ],
        "terrain": {"vertices": [-10, -10, 0, 40, -10, 0, 40, 40, 0, -10, 40, 0], "triangles": [0, 1, 2, 0, 2, 3]},
        "baseHeight": 8.5,
        "runPerRise": 0.5,
        "capZ": 25,
        "capped": True,
        "baseDepth": 1,
        "originAltitude": 100,
    }


def test_native_snapshot_returns_closed_display_and_mean_only_figures():
    original = request()
    before = copy.deepcopy(original)
    result = calculate(original)
    assert original == before
    assert result["parcelArea"] == pytest.approx(400)
    assert result["allowedArea"] == pytest.approx(196)
    assert result["meanZ"] == 0
    assert result["meanASL"] == 100
    assert len(result["references"]) == 12
    assert len(result["normals"]) == len(result["vertices"])
    assert len(result["triangles"]) % 3 == 0
    assert result["faces"] > 0
    assert not {"min", "max", "std", "gfa", "far"} & result.keys()
    json.dumps(result, allow_nan=False)


def test_same_shared_solver_handles_simple_concavity():
    result = calculate(request(True))
    assert result["allowedArea"] == pytest.approx(460)
    assert result["faces"] <= 256


def test_sloped_boundary_and_mean_use_project_origin_altitude_once():
    data = request()
    vertices = data["terrain"]["vertices"]
    for i in range(0, len(vertices), 3):
        vertices[i + 2] = 5 + 0.25 * vertices[i] + 0.1 * vertices[i + 1]
    result = calculate(data)
    assert result["meanZ"] == pytest.approx(8.5)
    assert result["meanASL"] == pytest.approx(108.5)
    for line in result["boundary"]:
        for x, y, z in zip(line[::3], line[1::3], line[2::3], strict=True):
            assert z == pytest.approx(5 + 0.25 * x + 0.1 * y)
    data["originAltitude"] = None
    assert calculate(data)["meanASL"] is None


def test_concave_winding_reversal_does_not_change_footprint_or_mean():
    data = request(True)
    before = calculate(data)
    data["edges"] = [{**edge, "a": edge["b"], "b": edge["a"]} for edge in reversed(data["edges"])]
    after = calculate(data)
    assert after["parcelArea"] == pytest.approx(before["parcelArea"])
    assert after["allowedArea"] == pytest.approx(before["allowedArea"])
    assert after["meanASL"] == pytest.approx(before["meanASL"])


def test_zero_setback_retains_height_regulation_and_reference_flags_are_run_local():
    data = request()
    for edge in data["edges"]:
        edge["distance"] = 0
        edge["reference"] = False
    result = calculate(data)
    assert result["allowedArea"] == pytest.approx(400)
    assert result["meanZ"] is None and result["meanASL"] is None
    assert not result["references"]
    assert min(result["vertices"][2::3]) == -1
    assert max(result["vertices"][2::3]) <= 25


def test_none_height_rule_is_distinct_from_zero_offset_and_cap_can_bound_it():
    data = request()
    for edge in data["edges"]:
        edge["regulated"] = False
    result = calculate(data)
    assert result["allowedArea"] == pytest.approx(400)
    assert max(result["vertices"][2::3]) == 25
    data["capped"] = False
    with pytest.raises(ValueError, match="unbounded"):
        calculate(data)


@pytest.mark.parametrize("mutation", ["review", "arc", "index", "nan", "gap", "collapse"])
def test_invalid_or_unsupported_snapshot_refuses_instead_of_falling_back(mutation):
    data = request()
    if mutation == "review":
        data["edges"][0]["review"] = True
    if mutation == "arc":
        data["edges"][0]["arc"] = 0.5
    if mutation == "index":
        data["terrain"]["triangles"][0] = 999
    if mutation == "nan":
        data["terrain"]["vertices"][0] = float("nan")
    if mutation == "gap":
        data["terrain"]["vertices"][3] = 5
        data["terrain"]["vertices"][6] = 5
    if mutation == "collapse":
        data = request(True)
        for edge in data["edges"]:
            edge["distance"] = 6
    with pytest.raises((ValueError, RuntimeError)):
        calculate(data)


def test_existing_bounded_bridge_can_call_shared_solver_without_a_command_or_ui():
    result = run(
        "from tapioca.massing.native import calculate\npayload = calculate(request)",
        "<native Massing calculation>",
        {"request": request()},
        [{"portId": "payload"}],
        [],
        15000,
    )
    assert result["ok"], result["error"]
    assert result["outputs"]["payload"]["allowedArea"] == pytest.approx(196)


def test_automatic_preview_returns_hud_inset_without_a_fake_terrain_datum():
    data = request()
    data["terrain"] = None
    before = copy.deepcopy(data)
    result = preview(data)
    assert data == before
    assert result["hasEnvelope"] is False
    assert result["allowedArea"] == pytest.approx(196)
    assert len(result["offsetXY"]) == 8
    assert result["note"]
    assert result["vertices"] == result["triangles"] == result["offsets"] == result["boundary"] == []
    assert result["meanZ"] is None and result["meanASL"] is None
    json.dumps(result, allow_nan=False)


def test_automatic_preview_inset_changes_immediately_with_unsaved_offset():
    data = request()
    data["terrain"] = None
    before = preview(data)
    data["edges"][0]["distance"] = 1
    after = preview(data)
    assert after["offsetXY"] != before["offsetXY"]
    assert after["allowedArea"] == pytest.approx(224)


def test_offset_lines_follow_the_same_sloping_terrain_as_property_lines():
    data = request()
    vertices = data["terrain"]["vertices"]
    for i in range(0, len(vertices), 3):
        vertices[i + 2] = 5 + 0.25 * vertices[i] + 0.1 * vertices[i + 1]
    result = preview(data)
    assert result["hasEnvelope"]
    assert result["offsets"]
    for line in result["offsets"]:
        for x, y, z in zip(line[::3], line[1::3], line[2::3], strict=True):
            assert z == pytest.approx(5 + 0.25 * x + 0.1 * y)


def test_partial_unbounded_preview_keeps_site_lines_but_never_a_shell():
    data = request()
    data["capped"] = False
    for edge in data["edges"]:
        edge["regulated"] = False
    result = preview(data)
    assert not result["hasEnvelope"]
    assert "unbounded" in result["note"]
    assert result["boundary"] and result["offsets"]
    assert result["vertices"] == result["normals"] == result["triangles"] == []


def test_collapsed_inset_does_not_hide_available_property_boundary():
    data = request(True)
    for edge in data["edges"]:
        edge["distance"] = 6
    result = preview(data)
    assert not result["hasEnvelope"]
    assert result["boundary"]
    assert result["offsetXY"] == [] and result["offsets"] == []
    assert result["allowedArea"] == 0


def test_bounded_bridge_calls_partial_automatic_preview():
    data = request()
    data["terrain"] = None
    result = run(
        "from tapioca.massing.native import preview\npayload = preview(request)",
        "<native Massing calculation>",
        {"request": data},
        [{"portId": "payload"}],
        [],
        15000,
    )
    assert result["ok"], result["error"]
    assert not result["outputs"]["payload"]["hasEnvelope"]
    assert result["outputs"]["payload"]["offsetXY"]


def test_native_shell_intersection_fixtures_match_the_shared_python_solver():
    fixtures = json.loads((Path(__file__).parent / "cpp" / "fixtures" / "massing-shells.json").read_text())
    assert len(fixtures) == 2
    for fixture in fixtures:
        actual = calculate(fixture["request"])
        expected = fixture["payload"]
        for key in ("vertices", "normals", "offsetXY"):
            assert actual[key] == pytest.approx(expected[key], abs=1e-8)
        assert actual["triangles"] == expected["triangles"]
        assert actual["allowedArea"] == pytest.approx(expected["allowedArea"])
