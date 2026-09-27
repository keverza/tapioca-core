"""Keep the project API v2 schema literals valid and strict."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "ProjectCommands.cpp"
)
_QUERY_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "GhElementQueryCommands.cpp"
)
_BRIDGE_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "Grasshopper", "GhBridge.cpp"
)


def _source():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        return source.read()


def _schemas():
    return [json.loads(value) for value in re.findall(r'R"json\((.*?)\)json"', _source(), re.DOTALL)]


def _registration_count():
    return len(re.findall(r"&MakeRegisteredNativeCommand<", _source()))


def test_project_schemas_are_valid_strict_json_objects():
    schemas = _schemas()

    # Two per registration -- input and response, both mandatory. Derived rather than
    # a literal count so adding a command cannot leave a stale number here, while a
    # DROPPED schema (the thing this guards) still fails.
    assert len(schemas) == 2 * _registration_count()
    assert all(schema["type"] == "object" for schema in schemas)
    assert all(schema["additionalProperties"] is False for schema in schemas)


def test_place_info_schema_declares_all_time_overrides():
    place_input = json.loads(re.search(
        r'\{ "GetPlaceInfo",.*?R"json\((.*?)\)json"', _source(), re.DOTALL
    ).group(1))

    assert set(place_input["properties"]) == {"year", "month", "day", "hour", "minute", "second"}
    assert all(field["type"] == "integer" for field in place_input["properties"].values())


def test_gh2_element_query_is_bounded_and_reports_aligned_statuses():
    with open(_QUERY_SOURCE_PATH, encoding="utf-8") as source:
        query_source = source.read()
    match = re.search(
        r'\{ "GetGhElementQuery",.*?R"json\((.*?)\)json"\s*,\s*R"json\((.*?)\)json"',
        query_source, re.DOTALL,
    )
    assert match is not None
    request, response = (json.loads(part) for part in match.groups())
    assert request["properties"]["guids"]["maxItems"] == 8
    assert request["properties"]["selectors"]["maxItems"] == 64
    assert request["additionalProperties"] is False
    rows = response["properties"]["elements"]["items"]
    assert {"guid", "status", "diagnostic", "items", "more"} == set(rows["required"])
    assert request["properties"]["offset"]["maximum"] == 2048
    assert rows["properties"]["items"]["items"]["additionalProperties"] is False
    mesh_fields = rows["properties"]["items"]["items"]["properties"]
    assert mesh_fields["vertices"]["maxItems"] == 12288
    assert mesh_fields["triangles"]["maxItems"] == 24576


def test_gh2_bridge_admits_the_paged_query_shape():
    with open(_BRIDGE_SOURCE_PATH, encoding="utf-8") as source:
        bridge = source.read()
    admission = bridge.split('request.command == "Tapioca.GetGhElementQuery"', 1)[1]
    admission = admission.split('request.command == "Tapioca.SetSelection"', 1)[0]
    assert r'request.parameters.find (",\"offset\":")' in admission
    assert "request.parameters.back () == '}'" in admission
    assert 'request.parameters.compare (request.parameters.size () - 2, 2, "]}")' not in admission
    assert 'request.command == "Tapioca.CreateMesh"' in bridge
    assert '\\"skirt\\":\\"SurfaceOnlyWithoutSkirt\\"' in bridge


def test_project_success_schemas_forbid_legacy_fields_and_require_the_payload():
    for schema in _schemas()[1::2]:
        assert "ok" not in schema["properties"]
        assert "error" not in schema["properties"]
        optional = {"selectionStamp"} if "selectionStamp" in schema["properties"] else set()
        assert set(schema["required"]) == set(schema["properties"]) - optional
