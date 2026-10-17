"""Offline oracle regressions; no Archicad calls."""

from copy import deepcopy

import pytest

from tools.surface_fixture import triangle_area, validate_fixture, validate_result

GUID = "77ee2bcc-8b47-46de-8198-a243f487bf52"
SURFACE = "d8d02123-184a-4a7c-9330-d9926a11e899"


def fixture():
    return {
        "elementTypes": {GUID: "Slab"},
        "absoluteTolerance": 1e-6,
        "cases": [
            {
                "name": "analytic-square",
                "elementGuid": GUID,
                "coverGuids": [],
                "polygonPoints": [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0],
                "surfaceAttributeGuid": SURFACE,
                "expectedGrossAreaM2": 1,
                "expectedExposedAreaM2": 1,
                "basis": "Independent unit square",
            }
        ],
    }


def response():
    case = fixture()["cases"][0]
    source = {"elementId": {"guid": GUID}, "sourceBody": 1, "sourcePgon": 2}
    request = {
        "snapshotId": 1,
        "captureStamp": 3,
        "coverScopeId": "test:none",
        "faces": [source],
        "covers": [],
        "surfaceAttributeIndices": [7, 8],
    }
    totals = {"grossGeometricArea": 1, "exposedArea": 1, "selectedExposedArea": 1}
    data = {
        **request,
        "unit": "m2",
        "coordinateUnit": "m",
        "indexSpace": "ModelerAPI-1-based",
        "classification": "explicitSourceFaces-contactOnly",
        "algorithmVersion": 1,
        "areas": deepcopy(totals),
        "faces": [{"source": source, "materialPoolIndex": 19, "surfaceAttributeIndex": 7, "areas": deepcopy(totals)}],
        "elements": [{"elementId": {"guid": GUID}, "areas": deepcopy(totals)}],
        "surfaces": [
            {"surfaceAttributeIndex": 7, "areas": deepcopy(totals)},
            {"surfaceAttributeIndex": 8, "areas": dict.fromkeys(totals, 0)},
        ],
        "fragments": [
            {
                "source": source,
                "materialPoolIndex": 19,
                "surfaceAttributeIndex": 7,
                "selected": True,
                "areaM2": 1,
                "points": case["polygonPoints"],
                "triangles": [0, 1, 2, 0, 2, 3],
            }
        ],
        "statistics": {"outputTriangles": 2},
    }
    return data, request, case


def test_independent_fixture_and_triangle_oracle():
    validate_fixture(fixture())
    data, request, case = response()
    validate_result(data, request, case, 7, 1, 1e-6)
    assert triangle_area(data["fragments"][0]) == 1


@pytest.mark.parametrize(
    "field,value",
    [
        ("expectedGrossAreaM2", 0),
        ("expectedExposedAreaM2", -1),
        ("expectedExposedAreaM2", 2),
        ("expectedExposedAreaM2", float("nan")),
        ("expectedGrossAreaM2", True),
        ("basis", ""),
        ("elementGuid", SURFACE),
        ("surfaceAttributeGuid", "bad-guid"),
        ("polygonPoints", [0, 0, 0]),
        ("polygonPoints", [float("inf")] * 9),
        ("coverGuids", [SURFACE]),
        ("coverGuids", [GUID, GUID]),
    ],
)
def test_rejects_bad_expectations_before_launch(field, value):
    data = fixture()
    data["cases"][0][field] = value
    with pytest.raises(ValueError):
        validate_fixture(data)


@pytest.mark.parametrize("tolerance", [0, -1, 1, float("nan"), True])
def test_tolerance_is_strict(tolerance):
    data = fixture()
    data["absoluteTolerance"] = tolerance
    with pytest.raises(ValueError):
        validate_fixture(data)


@pytest.mark.parametrize("pieces", [[[]], [[float("nan")] * 9], "not-a-list", [[0] * 9] * 33])
def test_invalid_hole_contours_refuse(pieces):
    data = fixture()
    data["cases"][0]["polygonPieces"] = pieces
    with pytest.raises(ValueError):
        validate_fixture(data)


@pytest.mark.parametrize(
    "field,value",
    [
        ("captureStamp", 4),
        ("classification", "exterior"),
        ("covers", [{"elementId": {"guid": GUID}, "sourceBody": 1}]),
        ("fragments", []),
        ("surfaces", []),
        ("elements", []),
    ],
)
def test_refuses_drift_partial_results_and_false_classification(field, value):
    data, request, case = response()
    data[field] = value
    with pytest.raises(AssertionError):
        validate_result(data, request, case, 7, 1, 1e-6)


@pytest.mark.parametrize(
    "field,value",
    [
        ("selected", False),
        ("materialPoolIndex", 7),
        ("surfaceAttributeIndex", 19),
        ("areaM2", 2),
        ("triangles", [0, 1, 9]),
        ("points", [float("nan")] * 12),
    ],
)
def test_mesh_material_and_selection_must_agree(field, value):
    data, request, case = response()
    data["fragments"][0][field] = value
    with pytest.raises((AssertionError, ValueError)):
        validate_result(data, request, case, 7, 1, 1e-6)


def test_full_cover_accepts_empty_geometry_only_with_zero_totals():
    data, request, case = response()
    data["fragments"] = []
    data["statistics"]["outputTriangles"] = 0
    for totals in [
        data["areas"],
        data["faces"][0]["areas"],
        data["elements"][0]["areas"],
        data["surfaces"][0]["areas"],
    ]:
        totals.update(exposedArea=0, selectedExposedArea=0)
    validate_result(data, request, case, 7, 0, 1e-6)


def test_empty_surface_filter_selects_none_not_all():
    data, request, case = response()
    request["surfaceAttributeIndices"] = []
    data["surfaces"].pop()
    data["fragments"][0]["selected"] = False
    for totals in [
        data["areas"],
        data["faces"][0]["areas"],
        data["elements"][0]["areas"],
        data["surfaces"][0]["areas"],
    ]:
        totals["selectedExposedArea"] = 0
    validate_result(data, request, case, 7, 1, 1e-6)
