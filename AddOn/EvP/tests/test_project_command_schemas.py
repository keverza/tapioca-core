"""Keep the project API v2 schema literals valid and strict."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "ProjectCommands.cpp"
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
    # DROPPED schema (the thing this guards) still fails. The index-based tests below
    # depend on the same in/out pairing.
    assert len(schemas) == 2 * _registration_count()
    assert all(schema["type"] == "object" for schema in schemas)
    assert all(schema["additionalProperties"] is False for schema in schemas)


def test_place_info_schema_declares_all_time_overrides():
    place_input = _schemas()[4]

    assert set(place_input["properties"]) == {"year", "month", "day", "hour", "minute", "second"}
    assert all(field["type"] == "integer" for field in place_input["properties"].values())


def test_project_success_schemas_forbid_legacy_fields_and_require_the_payload():
    for schema in _schemas()[1::2]:
        assert "ok" not in schema["properties"]
        assert "error" not in schema["properties"]
        assert set(schema["required"]) == set(schema["properties"])
