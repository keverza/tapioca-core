"""Explicit analytic faces; does not assert exterior membership or schedule equivalence."""

import json

import pytest

from tools.archicad_client import LiveTestError, normalized_guid
from tools.surface_fixture import close, pin, positive_int, triangle_area, validate_fixture, validate_result


def pytest_generate_tests(metafunc):
    if "surface_case" not in metafunc.fixturenames:
        return
    runner = getattr(metafunc.config, "_tapioca_live_runner", None)
    if runner is None or runner.options.suite != "surface":
        metafunc.parametrize("surface_case", [None])
        return
    fixture = validate_fixture(json.loads(runner.options.fixture.read_text(encoding="utf-8")))
    metafunc.parametrize("surface_case", fixture["cases"], ids=[case["name"] for case in fixture["cases"]])


@pytest.fixture(scope="module")
def surface_capture(live_client, live_runner):
    options = live_runner.options
    fixture = validate_fixture(json.loads(options.fixture.read_text(encoding="utf-8")))
    live_client.verify_project(options.expected_project)
    live_client.tapioca_call("Tapioca.BuildSnapshot", {"scope": "all", "meta": "none"})
    info = live_client.tapioca_call("Tapioca.GetSnapshotInfo")
    identity = pin(info)
    requested = {normalized_guid(case["elementGuid"]) for case in fixture["cases"]}
    requested.update(normalized_guid(guid) for case in fixture["cases"] for guid in case["coverGuids"])
    captured = {normalized_guid(guid) for guid in info["guids"]}
    assert requested <= captured, "Explicit fixture geometry absent from capture"
    catalog = live_client.tapioca_call("Tapioca.GetSnapshotSurfaces", identity)
    assert pin(catalog) == identity and catalog["scope"] == "capturedProjectSurfaceCatalog"
    return fixture, identity, catalog["surfaces"]


def test_surface_contact_fragments(live_client, live_runner, surface_capture, surface_case):
    case = surface_case
    fixture, identity, catalog = surface_capture
    tolerance = fixture["absoluteTolerance"]
    options = live_runner.options
    call = live_client.tapioca_call

    def fresh():
        live_client.verify_project(options.expected_project)
        assert pin(call("Tapioca.GetSnapshotInfo")) == identity, "Capture changed before publication"

    fresh()
    # Match the C-contour diagnostic's homogeneous double arrays at the
    # GS::ObjectState SDK boundary, including integral-valued heights.
    contour = {"polygonPoints": [float(value) for value in case["polygonPoints"]]}
    if case.get("polygonPieces"):
        contour["polygonPieces"] = [[float(value) for value in piece] for piece in case["polygonPieces"]]
    match = call(
        "Tapioca.InspectSurfaceSourceFace", {**identity, "elementId": {"guid": case["elementGuid"]}, **contour}
    )
    assert pin(match) == identity and match["status"] == "matched", match
    assert match["inventoryScope"] == "queryBounds" and match["indexSpace"] == "ModelerAPI-1-based"
    assert normalized_guid(match["elementId"]["guid"]) == normalized_guid(case["elementGuid"])
    face = match["matchedFace"]
    close(face["areaM2"], case["expectedGrossAreaM2"], tolerance)
    source = {"elementId": {"guid": case["elementGuid"]}, **{key: face[key] for key in ("sourceBody", "sourcePgon")}}
    surfaces = [row for row in catalog if normalized_guid(row["guid"]) == normalized_guid(case["surfaceAttributeGuid"])]
    assert len(surfaces) == 1, "Expected independently identified surface is absent/ambiguous"
    surface = positive_int(surfaces[0]["surfaceAttributeIndex"])
    unused = next(row["surfaceAttributeIndex"] for row in catalog if row["surfaceAttributeIndex"] != surface)
    covers = []
    for guid in case["coverGuids"]:
        inventory = call("Tapioca.InspectSurfaceSourceFace", {**identity, "elementId": {"guid": guid}})
        assert pin(inventory) == identity and inventory["status"] == "enumerated", inventory
        assert inventory["inventoryScope"] == "element" and inventory["indexSpace"] == "ModelerAPI-1-based"
        assert normalized_guid(inventory["elementId"]["guid"]) == normalized_guid(guid)
        assert inventory["faces"], "No complete cover inventory; never omit an invalid cover"
        for body in sorted({positive_int(row["sourceBody"]) for row in inventory["faces"]}):
            covers.append({"elementId": {"guid": guid}, "sourceBody": body})
    assert len(covers) <= 128
    request = {
        **identity,
        "coverScopeId": "live-fixture:" + case["name"] + ":none",
        "faces": [source],
        "covers": [],
        "surfaceAttributeIndices": [surface, unused],
    }

    def calculate(payload, expected):
        fresh()
        result = call("Tapioca.CalculateSurfaceFragments", payload)
        validate_result(result, payload, case, surface, expected, tolerance)
        fresh()
        return result

    calculate(request, case["expectedGrossAreaM2"])
    request = {**request, "coverScopeId": "live-fixture:" + case["name"] + ":explicit", "covers": covers}
    first = calculate(request, case["expectedExposedAreaM2"])
    assert calculate(request, case["expectedExposedAreaM2"]) == first, "Identical captured request changed its result"
    calculate({**request, "surfaceAttributeIndices": []}, case["expectedExposedAreaM2"])
    # Native quantities remain separately recorded comparison data, not the oracle.
    quantities = call(
        "Tapioca.GetExposedSurfaceQuantities",
        {
            "elements": [{"elementId": {"guid": case["elementGuid"]}}],
            "coverElements": [{"elementId": {"guid": guid}} for guid in case["coverGuids"]],
        },
    )
    assert quantities["unit"] == "m2"
    fresh()


def test_surface_stale_pin_is_refused(live_client, surface_capture):
    _, identity, _ = surface_capture
    with pytest.raises(LiveTestError, match="stale"):
        live_client.tapioca_call(
            "Tapioca.GetSnapshotSurfaces", {**identity, "captureStamp": identity["captureStamp"] + 1}
        )
    assert pin(live_client.tapioca_call("Tapioca.GetSnapshotInfo")) == identity


def test_seeded_exterior_takeoff_against_independent_fixture(live_client, surface_capture):
    fixture, identity, catalog = surface_capture
    exterior = fixture.get("exterior")
    if exterior is None:
        pytest.skip("Fixture has no independently specified exterior region")
    call = live_client.tapioca_call
    info = call("Tapioca.GetSnapshotInfo")
    assert pin(info) == identity
    surfaces = []
    for guid in exterior["surfaceAttributeGuids"]:
        matches = [
            row["surfaceAttributeIndex"] for row in catalog if normalized_guid(row["guid"]) == normalized_guid(guid)
        ]
        assert len(matches) == 1
        surfaces.append(matches[0])
    request = {
        **identity,
        "targetGuids": [guid for guid, kind in zip(info["guids"], info["elemTypes"], strict=True) if kind == 1],
        "coverGuids": [guid for guid, kind in zip(info["guids"], info["elemTypes"], strict=True) if kind == 2],
        "coverScopeId": "Captured slabs + Extra covers:automated-independent-fixture",
        "seeds": exterior["seeds"],
        "surfaceAttributeIndices": surfaces,
        "surfaceColors": [0xE5484DFF, 0xF6C945FF][: len(surfaces)],
        "publishOverlay": False,
        "compareNative": True,
        "contactToleranceM": 0.0,
    }
    data = call("Tapioca.CalculateExteriorSurfaceTakeoff", request)
    assert pin(data) == identity and data["classification"] == "userExteriorSeeds-connectedCoplanar"
    assert data["overlayPublished"] is False and data["coverScopeId"] == request["coverScopeId"]
    assert {normalized_guid(g) for g in data["coverGuids"]} == {normalized_guid(g) for g in request["coverGuids"]}
    assert data["nativeComparisonAvailable"] is True and "NOT equivalent" in data["nativeComparisonNote"]
    assert [row["surfaceAttributeIndex"] for row in data["surfaces"]] == surfaces
    measured = dict.fromkeys(surfaces, 0.0)
    for mesh in data["meshes"]:
        area = triangle_area(mesh)
        close(area, mesh["areaM2"], 1e-6)
        measured[mesh["surfaceAttributeIndex"]] += area
    for row, expected, color in zip(
        data["surfaces"], exterior["expectedAreaM2"], request["surfaceColors"], strict=True
    ):
        close(row["areaM2"], expected, fixture["absoluteTolerance"])
        close(row["areaM2"], measured[row["surfaceAttributeIndex"]], 1e-6)
        assert row["rgba"] == color
    close(sum(measured.values()), data["selectedAreaM2"], 1e-6)
    assert data["selectedTriangleCount"] == sum(len(mesh["triangles"]) // 3 for mesh in data["meshes"])
    assert call("Tapioca.CalculateExteriorSurfaceTakeoff", request) == data
    assert pin(call("Tapioca.GetSnapshotInfo")) == identity


def test_all_wall_faces_match_complete_explicit_inventory(live_client, surface_capture):
    fixture, identity, catalog = surface_capture
    if not fixture.get("allWallSurfaces"):
        pytest.skip("Fixture does not request whole-wall inventory validation")
    call = live_client.tapioca_call
    info = call("Tapioca.GetSnapshotInfo")
    targets = [g for g, kind in zip(info["guids"], info["elemTypes"], strict=True) if kind == 1]
    covers = [g for g, kind in zip(info["guids"], info["elemTypes"], strict=True) if kind in (1, 2)]
    indices = []
    for guid in fixture["allWallSurfaces"]:
        matches = [
            row["surfaceAttributeIndex"] for row in catalog if normalized_guid(row["guid"]) == normalized_guid(guid)
        ]
        assert len(matches) == 1
        indices.append(matches[0])
    faces, bodies = [], []
    normals = set()
    for guid in covers:
        inventory = call("Tapioca.InspectSurfaceSourceFace", {**identity, "elementId": {"guid": guid}})
        assert inventory["status"] == "enumerated" and inventory["faces"], inventory
        bodies.extend(
            {"elementId": {"guid": guid}, "sourceBody": body}
            for body in sorted({face["sourceBody"] for face in inventory["faces"]})
        )
        if guid in targets:
            for face in inventory["faces"]:
                faces.append(
                    {"elementId": {"guid": guid}, "sourceBody": face["sourceBody"], "sourcePgon": face["sourcePgon"]}
                )
                normals.add(tuple(round(value, 3) for value in face["normal"]))
    assert len(normals) >= 6, "Fixture must exercise multiple wall planes, not a single facade"
    request = {
        **identity,
        "regionMode": "allWallFaces",
        "contactToleranceM": 0.001,
        "seeds": [],
        "targetGuids": targets,
        "coverGuids": covers,
        "coverScopeId": "Captured walls and slabs:automated-whole-wall",
        "surfaceAttributeIndices": indices,
        "publishOverlay": False,
        "compareNative": True,
    }
    data = call("Tapioca.CalculateExteriorSurfaceTakeoff", request)
    assert pin(data) == identity and data["classification"] == "allWallFaces-contactOnly"
    assert data["regionFaceCount"] == len(faces)
    assert {normalized_guid(g) for g in data["regionElementGuids"]} == {normalized_guid(g) for g in targets}
    explicit = call(
        "Tapioca.CalculateSurfaceFragments",
        {
            **identity,
            "faces": faces,
            "covers": bodies,
            "coverScopeId": request["coverScopeId"],
            "surfaceAttributeIndices": indices,
            "contactToleranceM": 0.001,
        },
    )
    # Independent region enumeration over every source face. Same math kernel,
    # so this is completeness/integration evidence, not an independent area oracle.
    close(data["selectedAreaM2"], explicit["areas"]["selectedExposedArea"], 1e-6)
    assert len(explicit["faces"]) == len(faces)
    measured = dict.fromkeys(indices, 0.0)
    reached = set()
    for mesh in data["meshes"]:
        area = triangle_area(mesh)
        close(area, mesh["areaM2"], 1e-6)
        measured[mesh["surfaceAttributeIndex"]] += area
        reached.add(normalized_guid(mesh["guid"]))
    assert len(reached) > 2, "Whole-wall fixture must highlight more than the old two-wall facade"
    for row in data["surfaces"]:
        close(row["areaM2"], measured[row["surfaceAttributeIndex"]], 1e-6)
    close(sum(measured.values()), data["selectedAreaM2"], 1e-6)
    assert call("Tapioca.CalculateExteriorSurfaceTakeoff", request) == data
    assert pin(call("Tapioca.GetSnapshotInfo")) == identity


def test_criteria_candidates_have_an_automatic_multimaterial_tolerant_capture(live_client, surface_capture):
    fixture, identity, catalog = surface_capture
    if not fixture.get("allWallSurfaces"):
        pytest.skip("Fixture does not request the criteria calculator")
    call = live_client.tapioca_call
    info = call("Tapioca.GetSnapshotInfo")
    targets = [guid for guid, kind in zip(info["guids"], info["elemTypes"], strict=True) if kind in (1, 2, 17, 19)]
    request = {
        **identity,
        "regionMode": "allElementFaces",
        "seeds": [],
        "targetGuids": targets,
        "coverGuids": targets,
        "coverScopeId": "Captured construction elements:criteria-test",
        "contactToleranceM": 0.001,
        "surfaceAttributeIndices": [],
        "surfaceColors": [],
        "publishOverlay": False,
        "compareNative": True,
    }
    data = call("Tapioca.CalculateExteriorSurfaceTakeoff", request)
    assert pin(data) == identity and data["classification"] == "allElementFaces-contactOnly"
    assert data["contactToleranceM"] == 0.001
    assert {normalized_guid(g) for g in data["regionElementGuids"]} == {normalized_guid(g) for g in targets}
    assert 3 <= len(data["surfaces"]) <= 64, "Fixture must exercise a list, not two finish slots"
    assert {row["surfaceAttributeIndex"] for row in data["surfaces"]} <= {
        row["surfaceAttributeIndex"] for row in catalog
    }
    measured = {row["surfaceAttributeIndex"]: 0.0 for row in data["surfaces"]}
    for mesh in data["meshes"]:
        area = triangle_area(mesh)
        close(area, mesh["areaM2"], 1e-6)
        measured[mesh["surfaceAttributeIndex"]] += area
    for row in data["surfaces"]:
        close(row["areaM2"], measured[row["surfaceAttributeIndex"]], 1e-6)
    close(sum(measured.values()), data["selectedAreaM2"], 1e-6)
    assert call("Tapioca.CalculateExteriorSurfaceTakeoff", request) == data
    assert pin(call("Tapioca.GetSnapshotInfo")) == identity
