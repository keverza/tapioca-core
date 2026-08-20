"""Contract checks for typed element Settings-ID reads and writes."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "IdentityCommands.cpp"
)


def _schemas():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        return [json.loads(value) for value in re.findall(
            r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_identity_commands_have_strict_typed_schema_pairs():
    get_input, get_output, set_input, set_output = _schemas()

    for schema in (get_input, get_output, set_input, set_output):
        assert schema["type"] == "object"
        assert schema["additionalProperties"] is False

    get_item = get_input["properties"]["elements"]["items"]
    assert get_item["required"] == ["elementId"]
    assert get_output["required"] == ["identities", "count"]

    write_item = set_input["properties"]["identities"]["items"]
    assert write_item["required"] == ["elementId", "value"]
    result = set_output["properties"]["results"]["items"]
    assert result["required"] == ["elementId", "succeeded"]
    assert "ok" not in set_output["properties"]
    assert "error" not in set_output["properties"]
