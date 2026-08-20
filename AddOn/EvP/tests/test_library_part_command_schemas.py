"""Contracts for library-part lookup, discovery, and placement."""
import json
import os
import re


_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "ElementReadCommands.cpp"
)
_PLACEMENT_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands",
    "LibraryObjectCommands.cpp"
)


def test_library_part_reads_have_strict_v2_schemas():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        schemas = [json.loads(value) for value in re.findall(
            r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]

    part_input, part_output, placed_input, placed_output = schemas[-4:]
    for schema in (part_input, part_output, placed_input, placed_output):
        assert schema["additionalProperties"] is False

    assert part_output["required"] == [
        "libraryPartName", "libInd", "sizeA", "sizeB", "paramCount"
    ]
    element = placed_output["properties"]["elements"]["items"]
    assert element["required"] == ["elementId"]
    assert placed_output["required"] == ["elements", "count"]
    assert "ok" not in part_output["properties"]
    assert "ok" not in placed_output["properties"]


def _schemas_by_command(path=None):
    """Every registration in a domain file, keyed by COMMAND NAME.

    ⚠️ BY NAME, NEVER BY POSITION. This started as `a, b = schemas` and broke the
    moment a second command joined the domain; re-indexing from the end then
    broke again when a third landed in front. Both failures were the TEST
    reacting to the file growing rather than to a contract changing, which is
    noise that trains you to edit the test without reading it.
    """
    with open(path or _PLACEMENT_SOURCE_PATH, encoding="utf-8") as source:
        text = source.read()
    pattern = (r'\{\s*"([A-Za-z0-9]+)"\s*,\s*&MakeRegisteredNativeCommand<[^>]+>\s*,\s*'
               r'(?:true|false)\s*,\s*R"json\((.*?)\)json"\s*,\s*R"json\((.*?)\)json"\s*\}')
    return {name: (json.loads(inp), json.loads(out))
            for name, inp, out in re.findall(pattern, text, re.DOTALL)}


def test_library_object_placement_has_strict_typed_v2_schema():
    placement_input, placement_output = _schemas_by_command()["PlaceLibraryObject"]
    assert placement_input["additionalProperties"] is False
    assert placement_output["additionalProperties"] is False
    assert placement_input["required"] == ["libraryPartNames", "x", "y"]
    assert placement_input["properties"]["inheritFrom"]["required"] == ["elementId"]
    assert placement_input["properties"]["parameters"]["items"]["required"] == ["name", "value"]
    assert placement_output["required"] == ["elementId", "libraryPartName", "libInd"]
    assert "ok" not in placement_output["properties"]


def test_list_library_parts_reports_the_identity_and_the_honest_total():
    listing_input, listing_output = _schemas_by_command()["ListLibraryParts"]
    assert listing_input["additionalProperties"] is False
    assert listing_output["additionalProperties"] is False

    # Every input is optional — an OBJECT catalogue is the common call, and
    # omitting `subtype` asks for exactly that.
    assert "required" not in listing_input
    assert listing_input["properties"]["limit"]["minimum"] == 1

    # ⚠️ `subtype` IS A PLAIN STRING AND MUST STAY ONE. It was briefly an enum of
    # the accepted spellings, and the schema validator then rejected
    # subtype="Door" upstream of ResolveSubtype with "$.subtype: value is not in
    # enum" — throwing away the sentence that explains openings are cut into a
    # host wall and says what to write instead. The scope boundary is prose the
    # user has to read, so ONE place owns it: ResolveSubtype, asserted below.
    assert "enum" not in listing_input["properties"]["subtype"]

    part = listing_output["properties"]["parts"]["items"]
    # ⚠️ `unID` AND `name` are BOTH required, and that pairing is the contract:
    # a document name is not unique (Archicad registers only the newest of a
    # duplicate), and a unID appears nowhere in Archicad's UI. A picker that
    # dropped either half would be ambiguous or unloggable.
    assert "unID" in part["required"]
    assert "name" in part["required"]
    # `missing` is required rather than defaulted, so a caller cannot place a
    # part whose .gsm is gone without having been told.
    assert "missing" in part["required"]
    # `treePath` is required because it is what lets a caller show the Library
    # Manager's own folders. Grouping by typeID instead was the first cut, and
    # the report was that the browser looked like nothing in Archicad.
    assert "treePath" in part["required"]
    assert part["properties"]["treePath"]["items"]["type"] == "string"
    # `library` and `embedded` are what the [Embedded|Loaded] root is built from,
    # and they are also how a wrong tree is diagnosed without opening the dialog.
    assert "library" in part["required"]
    assert "embedded" in part["required"]
    # `truncated` travels with `total`: a silently shortened list reads as
    # "the library does not have it".
    assert listing_output["required"] == ["parts", "total", "truncated"]
    assert "ok" not in listing_output["properties"]


def test_openings_are_refused_by_name_with_a_sentence_that_says_what_to_use():
    source = open(_PLACEMENT_SOURCE_PATH, encoding="utf-8").read()

    # The three out-of-scope spellings are recognised EXPLICITLY rather than
    # falling into the generic "unknown subtype" branch, because "not supported,
    # and here is why" is a different message from "you made a typo" — and a door
    # request is not a typo.
    assert 'wanted == "door" || wanted == "window" || wanted == "skylight"' in source
    assert "An opening is cut into a host " in source

    # The accepted spellings, so a picker cannot quietly lose one.
    for accepted in ('"object"', '"lamp"', '"zonestamp"', '"label"', '"all"'):
        assert 'wanted == %s' % accepted in source

    # Omitting the subtype means OBJECTS, not everything. The first cut listed
    # every registered library part and put surfaces, images, lamps and templates
    # in a picker whose question is "which object do I place".
    assert 'if (wanted.IsEmpty () || wanted == "object") {' in source


def test_only_placeable_non_template_parts_reach_the_picker():
    source = open(_PLACEMENT_SOURCE_PATH, encoding="utf-8").read()

    # ⚠️ THE LINE THE FIRST LIVE RUN WAS MISSING. Without it the catalogue
    # included macros, textures, list schemes and templates — none of which can
    # be placed — and the report was "data is all over the place".
    assert "if (!part.isPlaceable || part.isTemplate)" in source

    # The tree lookup runs on the SURVIVORS, after the filter. Reversing the two
    # would pay two modern-API calls for every part in a multi-thousand-part
    # library only to throw most of them away.
    filter_at = source.index("if (!part.isPlaceable || part.isTemplate)")
    lookup_at = source.index("ACAPI::Library::GetLibraryManager ()")
    assert filter_at < lookup_at
