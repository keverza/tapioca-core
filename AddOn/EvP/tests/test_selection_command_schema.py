"""Contract checks for the first typed-ID selection API conversion."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "SelectionCommands.cpp"
)


def _schemas():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        return [json.loads(value) for value in re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_get_selection_uses_strict_tapir_style_element_ids():
    input_schema, output_schema = _schemas()[:2]

    assert input_schema == {"type": "object", "properties": {}, "additionalProperties": False}
    element = output_schema["properties"]["elements"]["items"]
    assert element["required"] == ["elementId"]
    assert element["properties"]["elementId"]["required"] == ["guid"]
    assert output_schema["additionalProperties"] is False


def test_selection_writes_accept_typed_elements_and_return_typed_missing_ids():
    schemas = _schemas()
    for input_schema, output_schema in ((schemas[2], schemas[3]), (schemas[4], schemas[5])):
        assert input_schema["additionalProperties"] is False
        item = input_schema["properties"]["elements"]["items"]
        assert item["required"] == ["elementId"]
        assert item["properties"]["elementId"]["required"] == ["guid"]
        missing = output_schema["properties"]["missing"]["items"]
        assert missing["required"] == ["elementId"]
        assert "ok" not in output_schema["properties"]
        assert "error" not in output_schema["properties"]


def test_every_selection_command_has_a_strict_v2_schema_pair():
    schemas = _schemas()

    assert len(schemas) == 20
    for index, schema in enumerate(schemas):
        assert schema["type"] == "object"
        assert schema["additionalProperties"] is False
        if index % 2 == 1:
            assert "ok" not in schema["properties"]
            assert "error" not in schema["properties"]
