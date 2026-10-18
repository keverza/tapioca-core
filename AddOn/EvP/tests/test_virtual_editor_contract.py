"""Source/schema contracts for diagnostics that cannot run without the native host."""

import json
import re
from pathlib import Path


ROOT = Path(__file__).parents[1] / "Sources" / "AddOn"
SOURCE = ROOT / "NativeCommands" / "VirtualEditorCommands.cpp"


def _state_schema():
    schemas = re.findall(r'R"json\((.*?)\)json"', SOURCE.read_text(encoding="utf-8"), re.S)
    return json.loads(schemas[-1])["properties"]


def test_editor_state_mode_table_and_schema_cover_every_gizmo_mode():
    header = (ROOT / "Edit" / "EditGizmo.hpp").read_text(encoding="utf-8")
    modes = re.search(r"enum class GizmoMode[^\{]*\{([^}]+)\}", header)[1]
    expected = [mode.strip().lower() for mode in modes.split(",")]
    source = SOURCE.read_text(encoding="utf-8")
    table = re.search(r"const char\* modes\[\] = \{([^}]+)\}", source)[1]
    assert re.findall(r'"([a-z]+)"', table) == expected
    assert _state_schema()["gizmo"]["enum"] == expected


def test_editor_state_names_and_declares_kerb_drag():
    source = SOURCE.read_text(encoding="utf-8")
    assert re.search(r'case eh::Drag::Kerb:\s*return "kerb";', source)
    assert "kerb" in _state_schema()["drag"]["enum"]
