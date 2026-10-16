"""Pinned native fragment adapter contracts; not live SDK acceptance."""

import copy
import json
import re
from pathlib import Path

import jsonschema
import pytest

ROOT = Path(__file__).resolve().parents[1]
ADDON = ROOT / "Sources/AddOn"
SOURCE = ADDON / "NativeCommands/SurfaceFragmentCommands.cpp"


def schemas():
    return [json.loads(raw) for raw in re.findall(r'R"json\((.*?)\)json"', SOURCE.read_text(encoding="utf-8"), re.S)]


def request():
    return {
        "snapshotId": 81,
        "captureStamp": 4,
        "coverScopeId": "explicit-empty-covers/model-state-A",
        "faces": [{"elementId": {"guid": "ABC"}, "sourceBody": 1, "sourcePgon": 3}],
        "covers": [],
    }


def test_all_schemas_are_strict_valid_json_schema():
    for schema in schemas():
        jsonschema.Draft202012Validator.check_schema(schema)
        assert schema["additionalProperties"] is False
        assert "ok" not in schema["properties"]
        assert "error" not in schema["properties"]


def test_catalog_and_calculation_require_capture_pins_and_explicit_cover_scope():
    catalog, catalog_response, calculate, response = schemas()
    assert set(catalog["required"]) == {"snapshotId", "captureStamp"}
    assert set(calculate["required"]) == {"snapshotId", "captureStamp", "coverScopeId", "faces", "covers"}
    assert catalog_response["properties"]["scope"]["const"] == "capturedProjectSurfaceCatalog"
    assert response["properties"]["classification"]["const"] == "explicitSourceFaces-contactOnly"
    jsonschema.validate(request(), calculate)


@pytest.mark.parametrize("field", ["snapshotId", "captureStamp", "coverScopeId", "faces", "covers"])
def test_omitting_required_fields_is_not_a_fallback(field):
    payload = request()
    del payload[field]
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(payload, schemas()[2])


@pytest.mark.parametrize("indices", [[], [101], [101, 202]])
def test_project_surface_filters_allow_none_one_or_multiple(indices):
    payload = request()
    payload["surfaceAttributeIndices"] = indices
    jsonschema.validate(payload, schemas()[2])


@pytest.mark.parametrize("indices", [[0], [-1], [101, 101], ["Finish A"]])
def test_surface_filters_cannot_be_names_invalid_or_duplicate_indices(indices):
    payload = request()
    payload["surfaceAttributeIndices"] = indices
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(payload, schemas()[2])


@pytest.mark.parametrize("extra", [{"bodyIdx": 0}, {"ipgon": 47}, {"meshIndex": 0}, {"materialPoolIndex": 7}])
def test_native_picker_and_pool_indices_cannot_substitute_source_faces(extra):
    payload = request()
    payload["faces"][0].update(extra)
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(payload, schemas()[2])


def test_counts_are_bounded_and_empty_faces_are_not_automatic_region_expansion():
    schema = schemas()[2]
    for field, value in [
        ("faces", []),
        ("faces", request()["faces"] * 513),
        ("covers", [{"elementId": {"guid": "ABC"}, "sourceBody": 1}] * 129),
    ]:
        payload = request()
        payload[field] = value
        with pytest.raises(jsonschema.ValidationError):
            jsonschema.validate(payload, schema)


def test_fragment_response_preserves_source_surface_and_exact_measurement_mesh():
    schema = schemas()[3]
    areas = {"grossGeometricArea": 0.5, "exposedArea": 0.5, "selectedExposedArea": 0.5}
    source = request()["faces"][0]
    payload = {
        "snapshotId": 81,
        "captureStamp": 4,
        "algorithmVersion": 1,
        "coverScopeId": "explicit-empty-covers",
        "unit": "m2",
        "coordinateUnit": "m",
        "indexSpace": "ModelerAPI-1-based",
        "classification": "explicitSourceFaces-contactOnly",
        "areas": areas,
        "covers": [],
        "faces": [{"source": source, "materialPoolIndex": 7, "surfaceAttributeIndex": 101, "areas": areas}],
        "fragments": [
            {
                "source": source,
                "materialPoolIndex": 7,
                "surfaceAttributeIndex": 101,
                "selected": True,
                "areaM2": 0.5,
                "loops": [[0, 0, 0, 1, 0, 0, 0, 1, 0]],
                "points": [0, 0, 0, 1, 0, 0, 0, 1, 0],
                "triangles": [0, 1, 2],
            }
        ],
        "surfaces": [{"surfaceAttributeIndex": 101, "areas": areas}],
        "elements": [{"elementId": source["elementId"], "areas": areas}],
        "statistics": {"candidateCoverPairs": 0, "sectionCount": 0, "work": 20, "outputTriangles": 1},
    }
    jsonschema.validate(payload, schema)
    bad = copy.deepcopy(payload)
    bad["fragments"][0]["highlightOffset"] = 0.01
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(bad, schema)


def test_native_commands_never_acquire_live_model_and_recheck_after_serialization():
    source = SOURCE.read_text(encoding="utf-8")
    assert source.count("return false;") >= 2
    assert source.count("bool NeedsMainThread () const override") == 2
    assert "ACAPI_" not in source
    assert "AcquireCurrentModel" not in source
    assert "BuildSnapshot" not in source
    assert "MeshStore::Get ().Current () == snapshot" in source
    assert "MeshStore::Get ().CaptureStamp () == snapshot->captureStamp" in source
    assert source.index("auto output = DescribeResult") < source.index("if (cancelled ())")
    assert "processControl.TestBreak ()" in source
    assert "ResolveSurfaceSelection" in source and "ex::Calculate" in source
    registry = (ADDON / "NativeCommands/CommandRegistry.cpp").read_text(encoding="utf-8")
    assert "&GetSurfaceFragmentCommandRegistrations" in registry


def test_surface_provenance_is_captured_at_the_one_triangle_emission_site():
    extractor = (ADDON / "Geometry/GeometryExtractor.cpp").read_text(encoding="utf-8")
    assert "polygon.GetMaterial (&material);" in extractor
    assert "std::max<int32_t> (material.GetExternalReference (), 0)" in extractor
    assert "const int32_t surfaceAttribute = ReadPolygonSurfaceAttribute (polygon);" in extractor
    assert "matIdx.GetOriginalIndex ()" not in extractor
    assert "mesh.triSurfaceAttribute.push_back (surfaceAttribute);" in extractor
    assert "snap->surfaceAttributes = ReadSurfaceAttributes ();" in extractor
    assert "ACAPI_Attribute_GetAttributesByType (API_MaterialID, attributes)" in extractor
    sliced = (ADDON / "ArchViz/ExtractionThread.cpp").read_text(encoding="utf-8")
    assert "handle->surfaceAttributes = ReadSurfaceAttributes ();" in sliced
    assert "handle->materialIdentity = ReadMaterialIdentity (*model);" in sliced
    assert "handle->materialIdentity.empty ()" in sliced


def test_material_read_failure_retains_geometry_with_unknown_surface():
    extractor = (ADDON / "Geometry/GeometryExtractor.cpp").read_text(encoding="utf-8")
    helper = extractor.split("int32_t ReadPolygonSurfaceAttribute", 1)[1].split("bool ExtractElement", 1)[0]
    assert "catch (const GS::Exception&)" in helper
    assert "return 0;" in helper
    assert "continue;" not in helper
