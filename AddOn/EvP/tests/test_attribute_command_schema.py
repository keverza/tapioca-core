"""Keep the attribute commands' API v2 schema literals valid and strict."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "AttributeCommands.cpp"
)

# The literals appear in registration order: GetAttributeInfo's input/output
# pair, then ListAttributes'. Indexing rather than unpacking, so adding a third
# command is an ordinary edit here instead of an unrelated failure.
_GET_INPUT, _GET_OUTPUT, _LIST_INPUT, _LIST_OUTPUT = range(4)

_ATTRIBUTE_KINDS = [
    "layer",
    "pen",
    "fill",
    "lineType",
    "surface",
    "buildingMaterial",
    "composite",
    "profile",
]


def _schemas():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        return [json.loads(value) for value in re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_attribute_schemas_are_valid_and_strict():
    schemas = _schemas()
    assert len(schemas) == 4, "each registered attribute command contributes an input and an output schema"
    input_schema, output_schema = schemas[_GET_INPUT], schemas[_GET_OUTPUT]

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


def test_list_attributes_schemas_are_valid_and_strict():
    schemas = _schemas()
    input_schema, output_schema = schemas[_LIST_INPUT], schemas[_LIST_OUTPUT]

    assert input_schema["additionalProperties"] is False
    assert input_schema["required"] == ["kind"]
    assert input_schema["properties"]["kind"]["enum"] == _ATTRIBUTE_KINDS
    assert output_schema["additionalProperties"] is False
    assert output_schema["properties"]["kind"]["enum"] == _ATTRIBUTE_KINDS
    assert output_schema["required"] == ["kind", "count", "attributes"]

    row = output_schema["properties"]["attributes"]["items"]
    assert row["additionalProperties"] is False
    # `label` is what a person picks and `index` identifies the attribute; the
    # value that actually gets stored is `name` OR `number`, and which one it is
    # depends on the kind - so neither can be required.
    assert row["required"] == ["label", "index"]
    assert set(row["properties"]) == {
        "label", "name", "number", "index", "color", "hidden", "locked", "folder", "preview",
    }

    # The swatch is sent as a DEFINITION, never as an image: the 8x8 bit
    # pattern, the dash lengths, the skin thicknesses. That is what keeps a
    # whole fill list one small response and lets the client draw at any size.
    preview = row["properties"]["preview"]
    assert preview["additionalProperties"] is False
    assert preview["required"] == ["kind"]
    assert preview["properties"]["kind"]["enum"] == ["color", "pattern", "line", "composite"]
    pattern = preview["properties"]["pattern"]
    assert pattern["minItems"] == 8 and pattern["maxItems"] == 8, (
        "a fill pattern is eight rows of eight bits; a variable length would be "
        "a different thing than the one API_Pattern hands over"
    )
    assert pattern["items"]["maximum"] == 255
    skin = preview["properties"]["skins"]["items"]
    assert skin["additionalProperties"] is False
    # A skin always has a thickness; its colour comes from the building
    # material's cut-fill pen and may not resolve.
    assert skin["required"] == ["thickness"]


def test_the_pen_kind_is_the_one_keyed_by_number():
    """A pen IS its number; every other attribute is picked by name.

    Pinned because the node catalog's pen picker declares an integer parameter
    while every other picker declares a string, and the two halves have to agree
    about which kind that is.
    """
    source = open(_SOURCE_PATH, encoding="utf-8").read()
    listing = source.split("class ListAttributesCommand")[1]
    pen_branch = listing.split('if (kind == "pen")')[1].split('API_AttrTypeID typeID;')[0]
    assert 'row.Add ("number"' in pen_branch
    assert 'row.Add ("name"' not in pen_branch
    named_branch = listing.split("API_AttrTypeID typeID;")[1]
    assert 'row.Add ("name"' in named_branch
    assert 'row.Add ("number"' not in named_branch
