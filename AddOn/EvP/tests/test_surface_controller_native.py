"""S1 native controller seams; lifecycle behavior is covered by ControllerFixture."""

import json
import re
from pathlib import Path

import jsonschema

ROOT = Path(__file__).parents[1] / "Sources/AddOn"
SOURCE = (ROOT / "NativeCommands/SurfaceTakeoffControllerCommands.cpp").read_text(encoding="utf-8")


def schema(name):
    return json.loads(re.search(rf'const char {name}\[\]\s*=\s*R"json\((.*?)\)json"', SOURCE, re.S)[1])


def test_controller_commands_have_strict_schemas_and_registration():
    for name in ("kStartOutput", "kPollOutput", "kEmptyInput", "kCancelOutput"):
        data = schema(name)
        jsonschema.Draft202012Validator.check_schema(data)
        assert data["additionalProperties"] is False
    assert schema("kPollOutput")["properties"]["state"]["enum"] == [
        "empty",
        "pending",
        "current",
        "stale",
        "refused",
        "cancelled",
    ]
    registry = (ROOT / "NativeCommands/CommandRegistry.cpp").read_text(encoding="utf-8")
    assert "&GetSurfaceTakeoffControllerCommandRegistrations" in registry
    for command in ("StartExposedSurfaceTakeoff", "PollExposedSurfaceTakeoff", "CancelExposedSurfaceTakeoff"):
        assert f'return "{command}"' in SOURCE


def test_controller_has_no_overlay_dependency_and_checks_capture_before_publication():
    assert "Overlay3D" not in SOURCE and "ArchViz/Dxgi/" not in SOURCE
    controller = (ROOT / "Geometry/SurfaceTakeoffController.cpp").read_text(encoding="utf-8")
    assert "ticket->generation == generation_" in controller
    assert "Current (ticket->request.snapshot)" in controller
    assert "lastGood_" in controller and "ControllerState::Stale" in controller
    assert "ticket->cancelled.load ()" in controller


def test_clear_close_and_unload_revoke_native_controller():
    takeoff = (ROOT / "NativeCommands/SurfaceTakeoffCommands.cpp").read_text(encoding="utf-8")
    main = (ROOT / "AddOnMain.cpp").read_text(encoding="utf-8")
    assert "SharedSurfaceTakeoffController ().Clear ();" in takeoff
    assert main.count("SharedSurfaceTakeoffController ().OnProjectClosed ();") >= 3
    assert "SharedSurfaceTakeoffController ().Shutdown ();" in main
