"""Source contracts for SDK/DG lifecycle glue; pure stores are also C++-tested."""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "Sources/AddOn"


def test_transient_lifetime_is_explicit_and_backward_compatible():
    source = (ROOT / "NativeCommands/OverlayLayerCommands.cpp").read_text(encoding="utf-8")
    declaration = source[
        source.index("constexpr const char kSetOverlayLayerInput") : source.index(
            "constexpr const char kSetOverlayLayerOutput"
        )
    ]
    value = json.loads("".join(re.findall(r'R"json\((.*?)\)json"', declaration, re.S)))
    assert value["properties"]["lifetime"]["enum"] == ["project", "command"]
    assert "lifetime" not in value["required"]
    header = (ROOT / "ArchViz/OverlayLayers.hpp").read_text(encoding="utf-8")
    assert "LayerLifetime lifetime = LayerLifetime::Project" in header


def test_run_clears_before_workflow_or_python_launch():
    source = (ROOT / "Palette/ControlPaletteRun.cpp").read_text(encoding="utf-8")
    body = source[source.index("void ControlPalette::RunSelected") :]
    assert body.index("ForgetCommandPreviews ()") < body.index("IsWorkflowCommand (info)")
    cleanup = source[
        source.index("void ControlPalette::ForgetCommandPreviews") : source.index("void ControlPalette::RunSelected")
    ]
    assert "RetainedPreviewStore::Get ().Clear ()" in cleanup
    assert "preview.ForgetContext ()" in cleanup
    assert "ClearCommandPreviews ()" in cleanup
    controller = (ROOT / "ArchViz/OverlayControllerHud.cpp").read_text(encoding="utf-8")
    assert "ClearCommandScoped ()" in controller
    assert "StopRoofEditor" in controller


def test_new_open_close_release_hidden_palette_copies_and_store():
    main = (ROOT / "AddOnMain.cpp").read_text(encoding="utf-8")
    new_open = main[main.index("case APINotify_New:") : main.index("case APINotify_Close:")]
    close = main[main.index("case APINotify_Close:") : main.index("case APINotify_Quit:")]
    for event in (new_open, close):
        assert "ForgetCommandPreviews ()" in event
        assert "OnProjectClosed ()" in event
    release = (ROOT / "ArchViz/OverlayRelease.cpp").read_text(encoding="utf-8")
    assert "RetainedPreviewStore::Get ().Clear ()" in release
    assert "overlaylayers::ClearEverything ()" in release
    panel = (ROOT / "Palette/PreviewPanelLifecycle.cpp").read_text(encoding="utf-8")
    assert "ClearTrace ()" in panel
    assert "ClearOverlay ()" in panel
    assert "Request3DHost (previewpanel::Host::None)" in panel


def test_model_plane_text_is_not_a_screen_sized_annotation():
    header = (ROOT / "ArchViz/OverlayLayers.hpp").read_text(encoding="utf-8")
    assert "normal" in header and "sizeMetres" in header
    reader = (ROOT / "NativeCommands/OverlayLayerReading.cpp").read_text(encoding="utf-8")
    assert 'ReadDouble (plane, "sizeMetres", text.sizeMetres)' in reader
