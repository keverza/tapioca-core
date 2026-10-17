"""Native seam assertions supplement, never replace, SDK/live tests."""

import json
import re
from pathlib import Path

import jsonschema
import pytest

ROOT = Path(__file__).parents[1] / "Sources/AddOn"
SOURCE = (ROOT / "NativeCommands/SurfaceTakeoffCommands.cpp").read_text(encoding="utf-8")


def schema(name):
    return json.loads(re.search(rf'const char {name}\[\]\s*=\s*R"json\((.*?)\)json"', SOURCE, re.S)[1])


def test_strict_takeoff_schemas_and_native_registration():
    for name in ("kInput", "kOutput", "kEmptyInput", "kClearOutput"):
        data = schema(name)
        jsonschema.Draft202012Validator.check_schema(data)
        assert data["additionalProperties"] is False
    registry = (ROOT / "NativeCommands/CommandRegistry.cpp").read_text(encoding="utf-8")
    assert "&GetSurfaceTakeoffCommandRegistrations" in registry
    assert 'return "CalculateExteriorSurfaceTakeoff"' in SOURCE
    assert "bool NeedsMainThread () const override" in SOURCE and "MainThreadGate::Get ().Invoke" in SOURCE
    assert "[snapshot, presentation, publication, generation, ticket]" in SOURCE
    assert "RunEnded (generation)" in SOURCE and "view::BeginRequest ()" in SOURCE
    assert "ArchViz/Dxgi/" not in SOURCE


@pytest.mark.parametrize("missing", schema("kInput")["required"])
def test_required_scope_cannot_be_omitted(missing):
    payload = dict(
        snapshotId=1,
        captureStamp=1,
        targetGuids=["w"],
        coverGuids=[],
        coverScopeId="none",
        seeds=[dict(guid="w", point=[1, 0, 1], normal=[0, -1, 0])],
        surfaceAttributeIndices=[100],
        publishOverlay=False,
        compareNative=False,
    )
    payload.pop(missing)
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(payload, schema("kInput"))


def test_overlay_lifetime_and_stale_result_guard_are_native():
    overlay = (ROOT / "ArchViz/SurfaceTakeoffOverlay.cpp").read_text(encoding="utf-8")
    assert "timer != s_timer" in overlay and "KillTimer" in overlay
    assert "request != s_request.load ()" in overlay
    assert "MeshStore::Get ().CaptureStamp ()" in overlay and "Model/capture changed" in overlay
    assert "surfacetakeoff::OnProjectClosed ();" in (ROOT / "ArchViz/OverlayRelease.cpp").read_text(encoding="utf-8")
    assert "if (cancelled ())" in SOURCE
