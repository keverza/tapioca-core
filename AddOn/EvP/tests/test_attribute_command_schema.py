"""Keep GetAttributeInfo's API v2 schema literals valid and strict."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "AttributeCommands.cpp"
)


def _schemas():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        return [json.loads(value) for value in re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_attribute_schemas_are_valid_and_strict():
    input_schema, output_schema = _schemas()

    assert input_schema["additionalProperties"] is False
    assert input_schema["required"] == ["name", "kind"]
    assert input_schema["properties"]["kind"]["enum"] == ["composite", "profile", "buildingMaterial"]
    assert output_schema["additionalProperties"] is False
    assert "ok" not in output_schema["properties"]
    assert "error" not in output_schema["properties"]
    assert set(output_schema["properties"]) == {
        "name", "kind", "index", "thickness", "height", "width",
    }
    assert output_schema["required"] == ["name", "kind", "index"]
