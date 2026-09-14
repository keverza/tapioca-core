import json
import os
import re

_SOURCE_PATH = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands", "CameraSetCommands.cpp"
)
_ADDON_PATH = os.path.join(os.path.dirname(__file__), "..", "Sources", "AddOn")


def _schemas():
    with open(_SOURCE_PATH, encoding="utf-8") as source:
        return [json.loads(value) for value in re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def test_camera_set_commands_have_strict_schema_pairs():
    schemas = _schemas()
    assert len(schemas) == 4
    for schema in schemas:
        assert schema["type"] == "object"
        assert schema["additionalProperties"] is False


def test_get_camera_set_returns_batch_capture_camera_shape():
    output = _schemas()[1]
    camera = output["properties"]["cameras"]["items"]
    assert camera["additionalProperties"] is False
    assert camera["required"] == [
        "valid",
        "source",
        "orthographic",
        "viewMoving",
        "eyeX",
        "eyeY",
        "eyeZ",
        "targetX",
        "targetY",
        "targetZ",
        "viewConeDegreesHorizontal",
        "sun",
    ]
    assert camera["properties"]["sun"]["required"] == ["enabled"]


def test_camera_mutations_require_declared_action_vocabulary():
    input_schema = _schemas()[2]
    assert input_schema["required"] == ["name", "action"]
    assert input_schema["properties"]["action"]["enum"] == [
        "add",
        "update",
        "remove",
        "restore",
        "clear",
    ]


def test_catalog_reads_and_serializes_camera_set_metadata():
    with open(os.path.join(_ADDON_PATH, "Python", "CommandCatalog.hpp"), encoding="utf-8") as source:
        header = source.read()
    with open(os.path.join(_ADDON_PATH, "Python", "CommandCatalog.cpp"), encoding="utf-8") as source:
        implementation = source.read()

    assert "GS::Array<GS::UniString> cameraSets;" in header
    assert 'os.Get ("camera_sets", info.cameraSets);' in implementation
    assert 'command.Add ("camera_sets", info.cameraSets);' in implementation


def test_palette_routes_camera_lists_and_clears_their_session_state():
    palette_path = os.path.join(_ADDON_PATH, "Palette")
    with open(os.path.join(palette_path, "ControlPalette.cpp"), encoding="utf-8") as source:
        shell = source.read()
    with open(os.path.join(palette_path, "ControlPaletteLayout.cpp"), encoding="utf-8") as source:
        layout = source.read()
    with open(os.path.join(palette_path, "CameraSetPanel.cpp"), encoding="utf-8") as source:
        panel = source.read()

    assert layout.index("selectionSets.PlaceAt") < layout.index("cameraSets.PlaceAt")
    assert layout.index("cameraSets.PlaceAt") < layout.index("params.PlaceAt")
    assert shell.count("cameraSets.Clear ();") == 4
    assert "cameraSets.HandleButtonClicked" in shell
    assert "cameraSets.HandleSelectionChanged" in shell
    assert "row.list->Attach (listObserver)" in panel
    assert panel.count("->Attach (buttonObserver)") == 5
