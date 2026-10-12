"""Source-face inspection is pinned to an immutable snapshot, never native IDs."""

import json
import re
from pathlib import Path

import jsonschema
import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Sources/AddOn/NativeCommands/SurfaceSourceCommands.cpp"


def schemas():
    return [json.loads(raw) for raw in re.findall(r'R"json\((.*?)\)json"', SOURCE.read_text(), re.S)]


def test_inspection_schema_requires_snapshot_and_capture_identity():
    request, response = schemas()
    jsonschema.Draft202012Validator.check_schema(request)
    jsonschema.Draft202012Validator.check_schema(response)
    assert set(request["required"]) == {"elementId", "snapshotId", "captureStamp"}
    assert request["additionalProperties"] is False
    assert response["additionalProperties"] is False
    jsonschema.validate({"elementId": {"guid": "ABC"}, "snapshotId": 1, "captureStamp": 1}, request)


@pytest.mark.parametrize("extra", [{"bodyIdx": 1}, {"ipgon": 1}, {"sourceBody": 1}, {"sourcePgon": 1}])
def test_native_ids_cannot_shortcut_the_geometric_mapping(extra):
    request, _ = schemas()
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate({"elementId": {"guid": "ABC"}, "snapshotId": 1, "captureStamp": 1, **extra}, request)


def test_inspection_accepts_complete_native_convex_decomposition():
    request, _ = schemas()
    payload = {
        "elementId": {"guid": "ABC"},
        "snapshotId": 1,
        "captureStamp": 1,
        "polygonPieces": [[0, 0, 0, 1, 0, 0, 0, 1, 0]],
    }
    jsonschema.validate(payload, request)
    payload["polygonPieces"] = [[]]
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(payload, request)


def test_inspection_reads_the_pinned_snapshot_without_sdk_calls():
    source = SOURCE.read_text()
    assert re.search(r"NeedsMainThread\s*\(\s*\)\s*const\s+override\s*\{\s*return false;", source)
    assert "MeshStore::Get ().Current () != snapshot" in source
    assert "snapshot->captureStamp != static_cast<uint64_t> (captureStamp)" in source
    assert "surfacemap::Resolve (faces, query)" in source
    assert "ACAPI_" not in source
    assert "expectedBody" not in source
    assert "expectedPgon" not in source


def test_source_triangle_indices_are_not_render_triangle_indices():
    _, response = schemas()
    face = response["$defs"]["face"]
    assert set(face["required"]) == {"sourceBody", "sourcePgon", "areaM2", "centroid", "normal", "sourceTriangles"}
    assert face["properties"]["sourceTriangles"]["items"]["minimum"] == 0
    assert face["properties"]["sourceBody"]["minimum"] == 1
    assert response["properties"]["indexSpace"]["const"] == "ModelerAPI-1-based"


def test_query_inventory_is_explicitly_scoped_and_full_inventory_stays_strict():
    source = SOURCE.read_text()
    assert "surfacemap::EnumerateQuerySourceFaces (*mesh, query, &reason)" in source
    assert "surfacemap::EnumerateSourceFaces (*mesh, &reason)" in source
    _, response = schemas()
    assert response["properties"]["inventoryScope"]["enum"] == ["element", "queryBounds"]
    assert "inventoryScope" in response["required"]
