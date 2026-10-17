"""Independent expectations and response checks for explicit-face live tests.

This is a test oracle, not a clipping implementation or exterior classifier.
Fixture areas/contours must come from analytic geometry or independent model
reads, never from CalculateSurfaceFragments output.
"""

from __future__ import annotations

import math

from tools.archicad_client import normalized_guid


def number(value, minimum=0):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < minimum:
        raise ValueError("Expected a finite nonnegative number")
    return value


def positive_int(value):
    if type(value) is not int or value <= 0:
        raise ValueError("Expected a positive integer")
    return value


def validate_fixture(fixture):
    types = fixture["elementTypes"]
    if not isinstance(types, dict) or not 1 <= len(types) <= 32:
        raise ValueError("Expected 1..32 explicit fixture elements")
    guids = {normalized_guid(guid) for guid in types}
    supported = {"Wall", "Slab", "Window", "Door", "Morph"}
    if len(guids) != len(types) or any(kind not in supported for kind in types.values()):
        raise ValueError("Duplicate GUID or unsupported fixture element type")
    tolerance = number(fixture["absoluteTolerance"])
    if not 0 < tolerance <= 0.001:
        raise ValueError("Surface tolerance must be in (0, 0.001] m2")
    cases = fixture["cases"]
    if not isinstance(cases, list) or not 1 <= len(cases) <= 32:
        raise ValueError("Expected 1..32 explicit surface cases")
    names = set()
    for case in cases:
        if not isinstance(case["name"], str) or not case["name"] or case["name"] in names:
            raise ValueError("Case names must be nonempty and unique")
        names.add(case["name"])
        if normalized_guid(case["elementGuid"]) not in guids:
            raise ValueError("Target is absent from elementTypes")
        covers = case["coverGuids"]
        if not isinstance(covers, list) or len(covers) > 32 or any(normalized_guid(g) not in guids for g in covers):
            raise ValueError("Covers must be explicit fixture elements")
        if len({normalized_guid(g) for g in covers}) != len(covers):
            raise ValueError("Duplicate fixture covers")
        pieces = case.get("polygonPieces", [])
        if not isinstance(pieces, list) or len(pieces) > 32:
            raise ValueError("Expected a bounded hole/decomposition list")
        for points in [case["polygonPoints"], *pieces]:
            if not isinstance(points, list) or not 9 <= len(points) <= 1536 or len(points) % 3:
                raise ValueError("Expected a complete bounded xyz contour")
            for coordinate in points:
                number(coordinate, -math.inf)
        gross, exposed = number(case["expectedGrossAreaM2"]), number(case["expectedExposedAreaM2"])
        if gross <= 0 or exposed > gross or (not covers and exposed != gross):
            raise ValueError("Invalid independent gross/exposed expectation")
        normalized_guid(case["surfaceAttributeGuid"])
        if not isinstance(case["basis"], str) or not case["basis"].strip():
            raise ValueError("Record the independent analytic basis")
    return fixture


def pin(data):
    return {key: positive_int(data[key]) for key in ("snapshotId", "captureStamp")}


def source_key(source):
    return (
        normalized_guid(source["elementId"]["guid"]),
        positive_int(source["sourceBody"]),
        positive_int(source["sourcePgon"]),
    )


def body_key(body):
    return normalized_guid(body["elementId"]["guid"]), positive_int(body["sourceBody"])


def close(actual, expected, tolerance):
    if not math.isclose(number(actual), number(expected), abs_tol=tolerance, rel_tol=0):
        raise AssertionError(f"Area {actual!r} differs from independent/mesh expectation {expected!r} (m2)")


def triangle_area(fragment):
    points, triangles = fragment["points"], fragment["triangles"]
    if len(points) < 9 or len(points) % 3 or not triangles or len(triangles) % 3:
        raise ValueError("Missing/truncated fragment mesh")
    for value in points:
        number(value, -math.inf)
    if any(type(i) is not int or not 0 <= i < len(points) // 3 for i in triangles):
        raise ValueError("Invalid fragment triangle index")
    areas = []
    for offset in range(0, len(triangles), 3):
        a, b, c = [points[i * 3 : i * 3 + 3] for i in triangles[offset : offset + 3]]
        u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
        area = math.hypot(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]) / 2
        if number(area) <= 0:
            raise ValueError("Degenerate fragment triangle")
        areas.append(area)
    return math.fsum(areas)


def validate_result(data, request, case, surface, expected_exposed, tolerance):
    for key in ("snapshotId", "captureStamp", "coverScopeId"):
        assert data[key] == request[key], f"Changed {key}"
    assert data["unit"] == "m2" and data["coordinateUnit"] == "m"
    assert data["indexSpace"] == "ModelerAPI-1-based"
    assert data["classification"] == "explicitSourceFaces-contactOnly" and data["algorithmVersion"] == 1
    assert sorted(map(body_key, data["covers"])) == sorted(map(body_key, request["covers"]))
    source = source_key(request["faces"][0])
    assert len(data["faces"]) == 1 and source_key(data["faces"][0]["source"]) == source
    face = data["faces"][0]
    pool = positive_int(face["materialPoolIndex"])
    assert face["surfaceAttributeIndex"] == surface
    selected = surface in request["surfaceAttributeIndices"]
    areas = []
    for fragment in data["fragments"]:
        assert source_key(fragment["source"]) == source
        assert fragment["materialPoolIndex"] == pool and fragment["surfaceAttributeIndex"] == surface
        assert fragment["selected"] is selected
        area = triangle_area(fragment)
        close(fragment["areaM2"], area, 1e-6)
        areas.append(area)
    exposed = math.fsum(areas)
    close(exposed, expected_exposed, tolerance)
    assert len(data["elements"]) == 1
    assert normalized_guid(data["elements"][0]["elementId"]["guid"]) == source[0]
    rows = [data["areas"], face["areas"], data["elements"][0]["areas"]]
    for totals in rows:
        close(totals["grossGeometricArea"], case["expectedGrossAreaM2"], tolerance)
        close(totals["exposedArea"], exposed, 1e-6)
        close(totals["selectedExposedArea"], exposed if selected else 0, 1e-6)
    expected_surfaces = {surface, *request["surfaceAttributeIndices"]}
    assert len(data["surfaces"]) == len(expected_surfaces)
    assert {row["surfaceAttributeIndex"] for row in data["surfaces"]} == expected_surfaces
    for row in data["surfaces"]:
        same = row["surfaceAttributeIndex"] == surface
        close(row["areas"]["grossGeometricArea"], case["expectedGrossAreaM2"] if same else 0, tolerance)
        close(row["areas"]["exposedArea"], exposed if same else 0, 1e-6)
        close(row["areas"]["selectedExposedArea"], exposed if same and selected else 0, 1e-6)
    assert data["statistics"]["outputTriangles"] == sum(len(f["triangles"]) // 3 for f in data["fragments"])
