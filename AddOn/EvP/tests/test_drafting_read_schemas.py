"""Contracts for typed text and arc drafting reads."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "DraftingCommands.cpp"
)


def test_drafting_reads_have_strict_typed_schema_pairs():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        schemas = [json.loads(value) for value in re.findall(
            r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]

    text_output_index = next(i for i, schema in enumerate(schemas)
                             if schema.get("required") == ["fromSelection", "texts", "count", "skipped"])
    arc_output_index = next(i for i, schema in enumerate(schemas)
                            if "arcs" in schema.get("properties", {}))
    text_input, text_output = schemas[text_output_index - 1:text_output_index + 1]
    arc_input, arc_output = schemas[arc_output_index - 1:arc_output_index + 1]
    for schema in schemas:
        assert schema["additionalProperties"] is False

    assert text_input["properties"]["elements"]["items"]["required"] == ["elementId"]
    assert text_output["properties"]["texts"]["items"]["required"][0] == "elementId"
    assert arc_input["properties"]["elements"]["items"]["required"] == ["elementId"]
    assert arc_output["properties"]["arcs"]["items"]["required"][0] == "elementId"
    assert "ok" not in text_output["properties"]
    assert "ok" not in arc_output["properties"]
