"""Source/schema contracts for diagnostics that cannot run without the native host."""

import json
import re
from pathlib import Path

ROOT = Path(__file__).parents[1] / "Sources" / "AddOn"
SOURCE = ROOT / "NativeCommands" / "VirtualEditorCommands.cpp"


def test_viewer_flyout_opens_editor_directly_through_the_main_thread_request():
    source = (ROOT / "ArchViz" / "DiligentHudShell.cpp").read_text(encoding="utf-8")
    select = source[source.index("const auto select =") : source.index("const auto dock =")]
    assert 'if (key == "@viewer.editor")' in select
    assert "vieweredit::RequestWorkflow (shell.dock, shell.fontStep)" in select
    assert "select (dock.selected)" in source
    assert "select (result.pressed)" in source
    assert 'Button ("Open virtual editor")' not in source


def test_native_editor_and_viewer_do_not_publish_the_navigation_command_strip():
    host = (ROOT / "ArchViz" / "OverlayHudHost.cpp").read_text(encoding="utf-8")
    assert 'spec.textInput = !own.viewerSurface && host.selected != std::string (editlayers::kLayer) + "#0"' in host
    navigation = (ROOT / "ArchViz" / "HudShellNavigation.cpp").read_text(encoding="utf-8")
    body = navigation[navigation.index("std::string NavigationInput (") :]
    assert body.index("if (!spec.textInput)") < body.index("ImGui::Separator ()")
    assert "Navigation commands only in this pass" not in body


def test_surface_switch_uses_deferred_owner_and_strict_transport_enums():
    source = SOURCE.read_text(encoding="utf-8")
    schemas = [json.loads(text) for text in re.findall(r'R"json\((.*?)\)json"', source, re.S)]
    assert "surface" in schemas[0]["properties"]["action"]["enum"]
    for schema in (schemas[0], schemas[1], schemas[-1]):
        assert schema["properties"]["surface"]["enum"] == ["overlay", "viewer"]
        assert schema["additionalProperties"] is False
    assert "archviz::surfaceswitch::Request" in source


def test_viewer_editor_stores_the_overlays_encoded_colours_through_a_unorm_view():
    # The overlay's prepared colours are sRGB-encoded; the viewer's swap-chain view is
    # _SRGB and would encode them twice (washed-out panels). Pin the UNORM twin, the
    # per-frame lifetime and the rebind of the viewer's own target.
    source = (ROOT / "ArchViz" / "ViewerEditorDraw.cpp").read_text(encoding="utf-8")
    for srgb, unorm in (("RGBA8_UNORM_SRGB", "RGBA8_UNORM"), ("BGRA8_UNORM_SRGB", "BGRA8_UNORM")):
        assert re.search(rf"case Diligent::TEX_FORMAT_{srgb}:\s*return Diligent::TEX_FORMAT_{unorm};", source)
    update = source[source.index("void ViewerEditorDraw::Update") : source.index("bool ViewerEditorDraw::Draw")]
    draw = update.index("Draw (device, context, bound->GetDesc ().Format")
    assert update.index("StoredEncodingView (target)") < draw
    assert update.count("SetRenderTargets (1, &target, depth") == 1
    assert update.index("SetRenderTargets (1, &target, depth") > draw
    assert "stored_" not in source  # never a retained swap-chain view


def test_landscape_commands_replace_the_deprecated_saved_set_selection_ui():
    panel = (ROOT / "ArchViz" / "HudVirtualEditorLandscapePanel.cpp").read_text(encoding="utf-8")
    for removed in (
        '"Sets"',
        '"New face set"',
        '"Set A (active)"',
        '"Set B (operand)"',
        '"Show set selection"',
        '"Output"',
    ):
        assert removed not in panel
    for kind in ("Union", "Difference", "Intersection"):
        assert f"BeginLandscapeCommand (s, lg::OperationKind::{kind})" in panel
    assert "LandscapeCommandPrompt (s)" in panel
    assert "BooleanLandscapeFaceSets (s," not in panel


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


def test_landscape_seed_export_and_state_are_strict_and_use_the_versioned_codec():
    source = SOURCE.read_text(encoding="utf-8")
    schemas = [json.loads(text) for text in re.findall(r'R"json\((.*?)\)json"', source, re.S)]
    seed = schemas[0]["properties"]["landscape"]
    assert seed["additionalProperties"] is False
    assert seed["properties"]["definition"] == {"type": "string", "maxLength": 8388608}
    document = schemas[1]["properties"]["document"]["properties"]
    assert document["landscape"]["type"] == "string"
    summary = document["landscapeSummary"]
    assert summary == _state_schema()["landscape"]
    assert summary["additionalProperties"] is False
    assert set(summary["required"]) == set(summary["properties"])
    assert summary["properties"]["regions"]["items"]["additionalProperties"] is False
    assert "landscapeFace" in _state_schema()["selectionKind"]["enum"]
    assert "ReadLandscape" in source and "WriteLandscape" in source
    assert "ACAPI_Element_Create" not in source
    assert "ACAPI_CallUndoableCommand" not in source


def test_landscape_live_request_fixture_matches_the_registered_input_contract():
    import jsonschema

    source = SOURCE.read_text(encoding="utf-8")
    schema = json.loads(re.findall(r'R"json\((.*?)\)json"', source, re.S)[0])
    fixture = json.loads((Path(__file__).parent / "fixtures" / "landscape-m1.json").read_text(encoding="utf-8"))
    jsonschema.validate(fixture["request"], schema)
    assert fixture["expected"]["sources"] == 6
    assert fixture["expected"]["propertyLineAreaM2"] == 24 * 16


def test_working_plane_start_and_state_fields_have_strict_schemas():
    source = SOURCE.read_text(encoding="utf-8")
    schemas = [json.loads(text) for text in re.findall(r'R"json\((.*?)\)json"', source, re.S)]
    start = schemas[0]
    assert start["properties"]["planeZ"] == {"type": "number"}
    assert start["properties"]["planeLocked"] == {"type": "boolean"}
    assert start["additionalProperties"] is False
    state = _state_schema()
    for name, kind in {
        "planeZ": "number",
        "planeLocked": "boolean",
        "planeSticky": "boolean",
        "planeCaptured": "boolean",
    }.items():
        assert state[name] == {"type": kind}
        assert f'result.Add ("{name}"' in source
    assert state["planeSource"]["enum"] == ["worldXY", "face", "pathPlan"]
    for name in ("planeOrigin", "planeU", "planeV", "planeNormal"):
        assert state[name] == {"$ref": "#Point3D"}
        assert f'result.Add ("{name}"' in source
    for name in ("planeSourceEntity", "planeSourceFace", "planeSourceRevision"):
        assert state[name] == {"type": "integer"}
        assert f'result.Add ("{name}"' in source
    assert state["planeSourceStation"] == {"type": "number", "minimum": 0}
    assert 'result.Add ("planeSourceStation", s->plane.sourceStation)' in source
