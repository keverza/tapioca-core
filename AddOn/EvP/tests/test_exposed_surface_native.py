"""Stage A exposed-surface Inspector: native command seam contracts.

These are the assertions that cannot be made by running the add-on: the exact SDK
calls behind Tapioca.GetExposedSurfaceQuantities and Tapioca.PickPolygon, the rule
that the picker never routes a human pick through MainThreadGate::Invoke, and the
strict wire schemas. The picker's pure state machine has its own offline C++ suite
(tests/cpp/test_polygonpickstate.cpp); this file is the C++/schema seam.
"""

import json
import re
from pathlib import Path

import jsonschema
import pytest

_ADDON = Path(__file__).parents[1] / "Sources" / "AddOn"
_NATIVE = _ADDON / "NativeCommands"
_INSPECTOR = _NATIVE / "SurfaceInspectorCommands.cpp"
_INSPECTOR_HEADER = _NATIVE / "SurfaceInspectorCommands.hpp"
_PICK_STATE = _NATIVE / "PolygonPickState.cpp"
_PICK_STATE_HEADER = _NATIVE / "PolygonPickState.hpp"


def _read(path):
    return path.read_text(encoding="utf-8")


def _squash(text):
    # clang-format may wrap a call or split a one-line body; the contracts below
    # are about tokens, not layout.
    return re.sub(r"\s+", " ", text)


def _schemas(source):
    return [json.loads(value) for value in re.findall(r'R"json\((.*?)\)json"', source, re.DOTALL)]


def test_quantities_wraps_the_verified_sdk_read_with_all_three_pointer_arguments():
    source = _read(_INSPECTOR)
    assert "ACAPI_Element_GetSurfaceQuantities (&targetGuids, &coverGuids, &quantities)" in _squash(source)
    # The quantity read is a main-thread read (default NeedsMainThread); it never
    # posts or invokes the gate itself.
    quantity = source.split("class GetExposedSurfaceQuantitiesCommand", 1)[1].split("// Tapioca.PickPolygon", 1)[0]
    assert "MainThreadGate" not in quantity
    assert "ACAPI_UserInput_GetPoint" not in quantity


def test_quantities_covers_are_explicit_and_never_default_to_the_whole_project():
    source = _read(_INSPECTOR)
    assert 'params.Get ("coverElements", requestedCovers)' in source
    assert "ACAPI_Element_GetElemList" not in source


def test_quantities_canonicalises_and_groups_by_canonical_guid():
    source = _read(_INSPECTOR)
    # Every identifier is round-tripped through the SDK's own parse/format before
    # it is used as a key, so "{ab..}" and "AB.." dedup and group together.
    assert "APIGuidFromString" in source
    assert "CanonicalGuid" in source
    assert "APIGuidToString (parsed)" in _squash(source)
    assert "byCanonical" in source
    assert "seenTargets.insert (ref.canonical)" in _squash(source)
    # Rows stay parallel to the request, original spelling preserved.
    assert "for (const ElementRef& ref : targets)" in _squash(source)


def test_picker_is_gate_free_and_posts_the_human_pick():
    source = _read(_INSPECTOR)
    picker = source.split("class PickPolygonCommand", 1)[1].split("// The live", 1)[0]
    squashed = _squash(picker)
    assert "bool NeedsMainThread () const override { return false; }" in squashed
    assert "MainThreadGate::Get ().Post" in squashed
    # A live pick must NEVER run inside a blocking Invoke (MainThreadGate.hpp rule).
    assert "MainThreadGate::Get ().Invoke" not in squashed
    assert "ACAPI_UserInput_GetPoint (&pointInfo, nullptr, &SurfacePickComponentProc)" in squashed


def test_picker_cancel_is_bound_to_the_exact_token():
    source = _read(_INSPECTOR)
    assert "ACAPI_UserInput_CancelUserInput ()" in _squash(source)
    # The posted cancel job carries the token/generation and refuses unless that
    # exact token still owns the running input.
    assert "if (!s_pickState.IsRunning (token))" in _squash(source)
    assert "PostCancelUserInput (token)" in _squash(source)


def test_picker_rejects_non_faces_and_checks_the_window_and_project():
    source = _read(_INSPECTOR)
    assert "ACAPI_Window_GetCurrentWindow" in source
    assert "APIWind_3DModelID" in source
    assert "ACAPI_ProjectOperation_Project" in source
    # guid != null but no polygon is a failure, never found:true.
    assert "pick.ipgon == 0" in _squash(source)
    assert "std::isfinite" in source


def test_picker_callback_is_bounded_and_identity_matched_in_the_pure_state():
    source = _read(_INSPECTOR)
    callback = source.rsplit("void SurfacePickComponentProc", 1)[1].split("\n}\n", 1)[0]
    assert "state->OnComponent (s_callbackToken, sighting)" in _squash(callback)
    assert "ArchVizLog" not in callback
    assert "ACAPI_" not in callback

    state = _read(_PICK_STATE)
    # The normal is attached ONLY when the live callback named the same component
    # (guid + dbElemIdx + bodyIdx + ipgon) and the normal was finite and non-zero.
    assert "lastSighting.guid == committed.guid" in _squash(state)
    assert "lastSighting.dbElemIdx == committed.dbElemIdx" in _squash(state)
    assert "lastSighting.ipgon == committed.ipgon" in _squash(state)
    assert "IsFiniteNormal (lastSighting.normal)" in _squash(state)


def test_pure_state_machine_is_sdk_free_and_pinnable_offline():
    header = _read(_PICK_STATE_HEADER)
    source = _read(_PICK_STATE)
    for text in (header, source):
        assert "ACAPinc" not in text
        assert "MainThreadGate" not in text
        assert re.search(r"ACAPI_\w+\s*\(", text) is None
    assert "void ResetSurfaceInspectorState ();" in _read(_INSPECTOR_HEADER)
    assert "void ResetSurfaceInspectorState ()" in _squash(_read(_INSPECTOR))


def test_registry_registers_the_inspector_domain():
    registry = _read(_NATIVE / "CommandRegistry.cpp")
    assert '#include "NativeCommands/SurfaceInspectorCommands.hpp"' in registry
    assert "&GetSurfaceInspectorCommandRegistrations," in registry
    assert "NativeCommandRegistrations GetSurfaceInspectorCommandRegistrations ()" in _squash(_read(_INSPECTOR))


def test_quantities_schema_is_strict_and_names_the_unit():
    schemas = _schemas(_read(_INSPECTOR))
    request = next(s for s in schemas if "action" not in s.get("properties", {}) and "coverElements" in s["properties"])
    response = next(s for s in schemas if "coverCount" in s.get("properties", {}))

    assert request["additionalProperties"] is False
    assert request["required"] == ["elements"]
    assert request["properties"]["elements"]["minItems"] == 1

    assert response["additionalProperties"] is False
    assert response["required"] == ["unit", "count", "coverCount", "coverElements", "elements"]
    assert response["properties"]["unit"]["const"] == "m2"

    quantity = response["$defs"]["quantity"]
    assert quantity["properties"]["unit"]["type"] == "string"
    assert quantity["properties"]["unit"]["const"] == "m2"
    assert quantity["required"] == ["materialIndex", "buildMatIdx", "exposedSurface", "unit"]
    assert set(("materialIndex", "materialName", "buildMatIdx", "buildingMaterialName")) <= set(quantity["properties"])

    row = response["$defs"]["row"]
    assert row["required"] == ["elementId", "found", "quantities"]

    assert "ok" not in response["properties"]
    assert "error" not in response["properties"]


def test_quantity_validation_keeps_real_native_failure_codes():
    source = _squash(_read(_INSPECTOR))
    assert len(re.findall(r'EVP_ACAPI_FAIL\s*\(\s*"ACAPI_Element_Get",\s*elementErr,', source)) == 2
    assert "quantity target %T" in source
    assert "quantity cover %T" in source


def test_input_guard_cannot_allocate_after_claiming_physical_input():
    source = _squash(_read(_INSPECTOR))
    guard = source.split("class ScopedPickInput {", 1)[1].split("};", 1)[0]
    assert "const polygonpick::Token& token;" in guard
    assert "const polygonpick::Token& token) noexcept" in guard


@pytest.mark.parametrize(
    "payload",
    [
        {"action": "start", "requestId": "client"},
        {"action": "poll"},
        {"action": "cancel", "requestId": "server", "prompt": "wrong phase"},
    ],
)
def test_picker_conditional_schema_rejects_wrong_phase_fields(payload):
    request = next(s for s in _schemas(_read(_INSPECTOR)) if "action" in s.get("properties", {}))
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(payload, request)


def test_picker_schema_forbids_client_request_id_on_start():
    schemas = _schemas(_read(_INSPECTOR))
    request = next(s for s in schemas if s.get("properties") and "action" in s["properties"])
    response = next(s for s in schemas if "state" in s.get("properties", {}))

    assert request["additionalProperties"] is False
    assert request["required"] == ["action"]
    assert request["properties"]["action"]["enum"] == ["start", "poll", "cancel"]

    branches = request["oneOf"]
    start_branch, poll_branch = branches
    # start permits only action and prompt -- the server owns requestId.
    assert start_branch["properties"]["action"] == {"const": "start"}
    assert "requestId" not in start_branch["properties"]
    assert start_branch["additionalProperties"] is False
    # poll/cancel require requestId and permit nothing else.
    assert poll_branch["required"] == ["action", "requestId"]
    assert "prompt" not in poll_branch["properties"]
    assert poll_branch["additionalProperties"] is False

    assert response["required"] == ["state", "requestId"]
    assert response["properties"]["state"]["enum"] == ["none", "pending", "done", "cancelled", "failed"]

    pick = response["$defs"]["pick"]
    for field in ("found", "guid", "dbElemIdx", "bodyIdx", "ipgon", "pos", "neig"):
        assert field in pick["properties"]
    assert "normal" not in pick["required"]
    assert pick["properties"]["normalSource"]["enum"] == ["callback"]

    assert "ok" not in response["properties"]
    assert "error" not in response["properties"]
