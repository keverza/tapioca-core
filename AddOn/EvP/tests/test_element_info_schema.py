"""Contract checks for the typed GetElementInfo conversion."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "ElementReadCommands.cpp"
)


def test_get_element_info_has_strict_typed_schema_pair():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        schemas = [json.loads(value) for value in re.findall(
            r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]

    input_schema, output_schema = schemas[:2]
    assert input_schema["additionalProperties"] is False
    assert input_schema["required"] == ["elements"]
    item = input_schema["properties"]["elements"]["items"]
    assert item["required"] == ["elementId"]

    record = output_schema["properties"]["infoOfElements"]["items"]
    assert record["required"] == ["elementId", "found", "type", "floorInd", "angle"]
    assert output_schema["required"] == ["infoOfElements", "count"]
    assert "ok" not in output_schema["properties"]
    assert "error" not in output_schema["properties"]
