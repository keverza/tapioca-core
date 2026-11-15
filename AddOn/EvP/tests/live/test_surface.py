"""Explicit analytic faces; does not assert exterior membership or schedule equivalence."""

import json
import os
import time
from pathlib import Path

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
    for row, expected in zip(data["surfaces"], exterior["expectedAreaM2"], strict=True):
        close(row["areaM2"], expected, fixture["absoluteTolerance"])
        close(row["areaM2"], measured[row["surfaceAttributeIndex"]], 1e-6)
        assert row["rgba"] == 0x5A7F62FF  # legacy per-material colors cannot override selection styling
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
    assert 3 <= len(data["surfaces"]) <= 512, "Fixture must exercise a list, not two finish slots"
    assert {row["rgba"] for row in data["surfaces"]} == {0x5A7F62FF}
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


def test_captured_door_window_bodies_remove_contact_without_seo(live_client, surface_capture):
    fixture, identity, _ = surface_capture
    if not fixture.get("allWallSurfaces"):
        pytest.skip("Fixture does not request the criteria calculator")
    call = live_client.tapioca_call
    info = call("Tapioca.GetSnapshotInfo")
    typed = list(zip(info["guids"], info["elemTypes"], strict=True))
    targets = [guid for guid, kind in typed if kind in (1, 2, 17, 19)]
    covers = [guid for guid, kind in typed if kind in (1, 2, 13, 14, 17, 19)]
    assert len(covers) > len(targets), "Fixture must include captured door/window geometry"
    request = {
        **identity,
        "regionMode": "allElementFaces",
        "seeds": [],
        "targetGuids": targets,
        "coverGuids": targets,
        "coverScopeId": "Construction baseline:casing-test",
        "contactToleranceM": 0.001,
        "surfaceAttributeIndices": [],
        "publishOverlay": False,
        "compareNative": False,
    }
    baseline = call("Tapioca.CalculateExteriorSurfaceTakeoff", request)
    request.update(coverGuids=covers, coverScopeId="Captured construction + doors/windows:casing-test")
    covered = call("Tapioca.CalculateExteriorSurfaceTakeoff", request)
    assert covered["coverBodyCount"] > baseline["coverBodyCount"]
    assert pin(covered) == identity
    assert {normalized_guid(g) for g in covered["coverGuids"]} == {normalized_guid(g) for g in covers}
    assert covered["selectedAreaM2"] < baseline["selectedAreaM2"] - 1e-6, "Casing contact must remove actual area"
    close(sum(triangle_area(mesh) for mesh in covered["meshes"]), covered["selectedAreaM2"], 1e-6)
    assert call("Tapioca.CalculateExteriorSurfaceTakeoff", request) == covered
    assert pin(call("Tapioca.GetSnapshotInfo")) == identity


def test_native_controller_calculates_with_injected_overlay_off(live_client, surface_capture):
    fixture, _, _ = surface_capture
    if not fixture.get("allWallSurfaces"):
        pytest.skip("Fixture does not request the criteria calculator")
    call = live_client.tapioca_call
    call("Tapioca.Overlay3D", {"action": "off"})
    prepared = call(
        "Tapioca.PrepareExposedSurfaceTakeoff",
        {"regionMode": "allElementFaces", "coverPolicy": "construction", "extraCoverGuids": [], "surfaceNames": []},
    )
    identity = pin(prepared)
    info = call("Tapioca.GetSnapshotInfo")
    targets = sorted(
        normalized_guid(guid).upper()
        for guid, kind in zip(info["guids"], info["elemTypes"], strict=True)
        if kind in (1, 2, 17, 19)
    )
    assert pin(info) == identity
    assert prepared["targetGuids"] == prepared["coverGuids"] == targets
    request = {
        **prepared,
        "seeds": [],
        "contactToleranceM": 0.001,
        "publishOverlay": False,
        "compareNative": False,
    }
    expected = call("Tapioca.CalculateExteriorSurfaceTakeoff", request)
    started = call("Tapioca.StartExposedSurfaceTakeoff", request)
    assert started["state"] == "pending" and started["generation"] > 0
    assert pin(started) == identity
    for _ in range(1000):
        polled = call("Tapioca.PollExposedSurfaceTakeoff")
        if polled["state"] != "pending":
            break
        time.sleep(0.01)
    else:
        pytest.fail("Native controller did not finish within 10 seconds")
    assert polled["state"] == "current", polled
    assert polled["generation"] == started["generation"]
    assert polled["hasLastGood"] is True
    assert polled["lastGoodGeneration"] == started["generation"]
    assert polled["lastGoodSnapshotId"] == identity["snapshotId"]
    assert polled["lastGoodCaptureStamp"] == identity["captureStamp"]
    assert polled["lastGoodResult"] == expected

    call("Tapioca.StartExposedSurfaceTakeoff", request)
    call("Tapioca.CancelExposedSurfaceTakeoff")
    for _ in range(1000):
        cancelled = call("Tapioca.PollExposedSurfaceTakeoff")
        if cancelled["state"] != "pending":
            break
        time.sleep(0.01)
    else:
        pytest.fail("Native controller cancellation did not settle within 10 seconds")
    assert cancelled["state"] in ("cancelled", "stale")
    assert cancelled["hasLastGood"] is True
    assert cancelled["lastGoodResult"] == expected
    assert pin(call("Tapioca.GetSnapshotInfo")) == identity


def test_seeded_start_refuses_a_replaced_prepared_capture(live_client, surface_capture):
    fixture, _, _ = surface_capture
    if not fixture.get("allWallSurfaces"):
        pytest.skip("Fixture does not request the criteria calculator")
    call = live_client.tapioca_call
    params = {"regionMode": "seededFacade", "coverPolicy": "extraOnly", "extraCoverGuids": [], "surfaceNames": []}
    prepared = call("Tapioca.PrepareExposedSurfaceTakeoff", params)
    replacement = call("Tapioca.PrepareExposedSurfaceTakeoff", params)
    assert replacement["snapshotId"] != prepared["snapshotId"]
    before = call("Tapioca.PollExposedSurfaceTakeoff")
    with pytest.raises(LiveTestError, match="Snapshot missing or stale"):
        call("Tapioca.StartSeededExposedSurfaceTakeoff", {**prepared, "seedCount": 1, "compareNative": False})
    assert call("Tapioca.PollExposedSurfaceTakeoff") == before


def test_document_launcher_publishes_the_accepted_controller_meshes(live_client, surface_capture):
    fixture, _, _ = surface_capture
    if not fixture.get("allWallSurfaces"):
        pytest.skip("Fixture does not request the criteria calculator")
    call = live_client.tapioca_call
    log_path = Path(os.environ["LOCALAPPDATA"]) / "Tapioca/logs/exposed_surface_calculator.log"
    log_offset = log_path.stat().st_size if log_path.exists() else 0
    call("Tapioca.ClearExteriorSurfaceTakeoff", {})
    call("Tapioca.Overlay3D", {"action": "off"})
    started = call(
        "Tapioca.RunCommand",
        {
            "folder": "ExposedSurfaceCalculator",
            "params": {
                "action": "Calculate",
                "object_scope": "Criteria - captured elements",
                "covering": "Captured construction + doors/windows",
                "contact_tolerance_mm": 1.0,
                "seed_faces": 1,
            },
        },
    )
    assert started["started"] is True and started["generation"] > 0
    try:
        for _ in range(1200):
            run = call("Tapioca.GetRunState", {})
            if run["generation"] == started["generation"] and not run["active"]:
                break
            time.sleep(0.05)
        else:
            pytest.fail("Document launcher workflow did not finish within 60 seconds")
        assert "No new surface takeoff" not in run["status"]
        assert "No new surface takeoff" not in run["resultText"]
        assert run["headers"] and run["rows"], run

        polled = call("Tapioca.PollExposedSurfaceTakeoff", {})
        assert polled["state"] == "current", polled
        result = polled["lastGoodResult"]
        assert result["nativeComparisonAvailable"] is False
        assert "main-thread publish command" in result["nativeComparisonNote"]
        assert result["selectedTriangleCount"] > 0 and result["selectedAreaM2"] > 0
        # Publication does not mutate the accepted controller DTO or let Python
        # substitute a second response payload.
        assert result["overlayPublished"] is False
        layers = call("Tapioca.OverlayLayers", {})["layers"]
        exposed = next(layer for layer in layers if layer["layer"] == "tapioca.exposedSurfaces")
        assert exposed["meshes"] == len(result["meshes"])
        assert exposed["triangles"] == result["selectedTriangleCount"]
        assert pin(call("Tapioca.GetSnapshotInfo")) == pin(result)
        with log_path.open("rb") as stream:
            stream.seek(log_offset)
            entries = [json.loads(line) for line in stream.read().decode("utf-8").splitlines() if line.startswith("{")]
        commands = [entry["command"] for entry in entries]
        assert "Tapioca.PrepareExposedSurfaceTakeoff" in commands
        assert "Tapioca.BuildSnapshot" not in commands
        assert "Tapioca.GetSnapshotSurfaces" not in commands
        assert "Tapioca.StartExposedSurfaceTakeoff" in commands
        assert "Tapioca.PollExposedSurfaceTakeoff" in commands
        assert "Tapioca.PublishExposedSurfaceTakeoff" in commands
        assert "Tapioca.CalculateExteriorSurfaceTakeoff" not in commands
        published = next(
            entry["data"] for entry in entries if entry["command"] == "Tapioca.PublishExposedSurfaceTakeoff"
        )
        assert published["result"]["nativeComparisonAvailable"] is True
        assert published["result"]["meshes"] == result["meshes"]
    finally:
        call("Tapioca.ClearExteriorSurfaceTakeoff", {})
        call("Tapioca.Overlay3D", {"action": "off"})


def test_bus_launch_preserves_and_restores_populated_role_stores(live_client, surface_capture):
    call = live_client.tapioca_call
    info = call("Tapioca.GetSnapshotInfo", {})
    wall = next(guid for guid, kind in zip(info["guids"], info["elemTypes"], strict=True) if kind == 1)
    commands = [json.loads(row) for row in call("Tapioca.GetCommands", {})["commands"]]
    exposed_path = next(row["path"] for row in commands if row["folder"] == "ExposedSurfaceCalculator")
    massing = next(row for row in commands if row["folder"] == "MassingFeasibility")
    massing_path = massing["path"]
    camera_role = massing["camera_sets"][0]

    def probe(store, owner, role, value=None):
        params = {"store": store, "owner": owner, "role": role}
        if value is not None:
            params["value"] = value
        return call("Tapioca.TestRoleStore", params)

    def finish(folder, params):
        started = call("Tapioca.RunCommand", {"folder": folder, "params": params})
        for _ in range(600):
            run = call("Tapioca.GetRunState", {})
            if run["generation"] == started["generation"] and not run["active"]:
                return run
            time.sleep(0.05)
        pytest.fail(f"{folder} did not finish within 30 seconds")

    assert probe("selection", exposed_path, "Extra covers", wall)["count"] == 1
    assert probe("selection", exposed_path, "Extra covers")["count"] == 1
    assert probe("camera", massing_path, camera_role, "role-test")["count"] == 1
    assert probe("camera", massing_path, camera_role)["count"] == 1
    finish("ExposedSurfaceCalculator", {"action": "Clear"})
    assert probe("selection", exposed_path, "Extra covers")["count"] == 1
    assert probe("camera", massing_path, camera_role)["count"] == 1
    finish("MassingFeasibility", {})
    assert probe("selection", exposed_path, "Extra covers")["count"] == 1
    assert probe("camera", massing_path, camera_role)["count"] == 1

    assert probe("selection", "palette-owner", "Extra covers", wall)["count"] == 1
    assert probe("camera", "palette-owner", "Reference", "role-test")["count"] == 1
    finish("ParkingSolver", {"action": "Clear preview"})
    restored = probe("selection", "palette-owner", "Extra covers")
    assert restored["owner"] == "palette-owner" and restored["count"] == 1
    restored_camera = probe("camera", "palette-owner", "Reference")
    assert restored_camera["owner"] == "palette-owner" and restored_camera["count"] == 1

    # Hold a different bus command open, then apply the exact project-boundary
    # adapter. The palette owner's declarations return, but old-document values
    # do not, and the temporary run cannot restore them again at completion.
    assert probe("selection", "palette-owner", "Extra covers", wall)["count"] == 1
    assert probe("camera", "palette-owner", "Reference", "role-test")["count"] == 1
    started = call("Tapioca.RunCommand", {"folder": "CommandRoleLifecycle", "params": {"duration_s": 10.0}})
    reset = call(
        "Tapioca.TestRoleStore",
        {"store": "selection", "owner": "temporary-owner", "role": "Extra covers", "projectReset": True},
    )
    assert reset["owner"] == "palette-owner" and reset["count"] == 0
    reset_camera = call(
        "Tapioca.TestRoleStore",
        {"store": "camera", "owner": "palette-owner", "role": "Reference"},
    )
    assert reset_camera["owner"] == "palette-owner" and reset_camera["count"] == 0
    call("Tapioca.CancelRun", {})
    for _ in range(600):
        run = call("Tapioca.GetRunState", {})
        if run["generation"] == started["generation"] and not run["active"]:
            break
        time.sleep(0.05)
    else:
        pytest.fail("Command role lifecycle probe did not cancel within 30 seconds")
    assert probe("selection", "palette-owner", "Extra covers")["count"] == 0
    assert probe("camera", "palette-owner", "Reference")["count"] == 0

    # A → temporary B → palette C → project reset: C is not replaced by A.
    started = call("Tapioca.RunCommand", {"folder": "CommandRoleLifecycle", "params": {"duration_s": 10.0}})
    assert probe("selection", "palette-c", "Current role", "current")["count"] == 1
    assert probe("camera", "palette-c", "Current camera", "current")["count"] == 1
    changed = call(
        "Tapioca.TestRoleStore",
        {"store": "selection", "owner": "temporary-owner", "role": "Current role", "projectReset": True},
    )
    assert changed["owner"] == "palette-c" and changed["count"] == 0
    changed_camera = probe("camera", "palette-c", "Current camera")
    assert changed_camera["owner"] == "palette-c" and changed_camera["count"] == 0
    call("Tapioca.CancelRun", {})
    for _ in range(600):
        run = call("Tapioca.GetRunState", {})
        if run["generation"] == started["generation"] and not run["active"]:
            break
        time.sleep(0.05)
    else:
        pytest.fail("Second command role lifecycle probe did not cancel within 30 seconds")
    assert probe("selection", "palette-c", "Current role")["count"] == 0
    assert probe("camera", "palette-c", "Current camera")["count"] == 0
