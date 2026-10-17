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


def test_all_wall_schema_allows_explicit_seedless_scope_but_not_unknown_modes():
    payload = dict(
        snapshotId=1,
        captureStamp=1,
        targetGuids=["w"],
        coverGuids=["w"],
        coverScopeId="all-walls",
        regionMode="allWallFaces",
        seeds=[],
        surfaceAttributeIndices=[100],
        publishOverlay=False,
        compareNative=False,
    )
    jsonschema.validate(payload, schema("kInput"))
    payload["regionMode"] = "guessExteriorFromMaterial"
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(payload, schema("kInput"))
    assert "snapshot->meshes[index].elemType != 1" in SOURCE
    assert "ex::ResolveAllTargetFaces" in SOURCE


def test_material_list_and_tolerance_contract_are_bounded():
    data = schema("kInput")
    assert data["properties"]["surfaceAttributeIndices"]["maxItems"] == 64
    assert data["properties"]["contactToleranceM"]["maximum"] == 0.001
    assert "allElementFaces" in data["properties"]["regionMode"]["enum"]
    assert "request.contactToleranceM = .001" in SOURCE
