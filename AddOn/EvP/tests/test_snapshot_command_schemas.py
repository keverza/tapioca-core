"""Keep the first API v2 native schemas valid and deliberately strict."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "SnapshotCommands.cpp"
)


def _schemas():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        return [json.loads(value) for value in re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_snapshot_schemas_are_valid_strict_json_objects():
    schemas = _schemas()

    assert len(schemas) == 8
    assert all(schema["type"] == "object" for schema in schemas)
    assert all(schema["additionalProperties"] is False for schema in schemas)


def test_build_snapshot_schema_models_its_public_inputs():
    build_input = _schemas()[0]

    assert build_input["properties"]["scope"]["enum"] == ["all", "selection"]
    assert build_input["properties"]["excludeTypes"]["items"]["type"] == "integer"
    assert build_input["properties"]["meta"]["oneOf"][1]["enum"] == ["none", "basic", "full"]


def test_snapshot_success_schemas_forbid_legacy_payload_fields():
    for schema in _schemas()[1::2]:
        assert "ok" not in schema["properties"]
        assert "error" not in schema["properties"]


def test_snapshot_success_schemas_declare_and_require_emitted_fields():
    build, release, status, info = _schemas()[1::2]

    assert set(build["properties"]) == {
        "snapshotId", "scope", "elementCount", "vertexCount", "triangleCount",
        "hasMetadata", "metaLevel", "metadataCancelled", "droppedElements",
        "droppedTriangles", "retainedBytes",
    }
    assert set(build["required"]) == set(build["properties"]) - {
        "droppedElements", "droppedTriangles",
    }
    assert set(release["properties"]) == {"freedBytes", "retainedBytes"}
    assert set(release["required"]) == set(release["properties"])
    assert set(status["required"]) == set(status["properties"])
    assert set(info["required"]) == set(info["properties"])
