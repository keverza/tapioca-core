"""The dry-run harness must reject what Archicad's dispatcher rejects.

WHY THIS TEST EXISTS.  On 2026-08-13 three consecutive camera-sync runs were lost
to input-schema mismatches -- `mode: "wakepredict"` absent from an enum, and
`intervalMs: 0` under a stated minimum.  Each cost a build, a sync, an Archicad
restart and a user round trip, and `dryrun_command.py` passed all three, because
it faked the wire and never consulted a schema.

⚠️ IT TESTS THE HARNESS, NOT ANY COMMAND.  Nothing here says a command is
correct.  It says that IF a command sends parameters the dispatcher would refuse,
the harness refuses them too -- offline, in a second, instead of in Archicad
after a rebuild.
"""

import importlib.util
import os
import sys

import pytest

_HARNESS = os.path.join(os.path.dirname(__file__), "dryrun_command.py")


def _load():
    spec = importlib.util.spec_from_file_location("dryrun_command", os.path.abspath(_HARNESS))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_schemas_are_actually_found():
    # If the registry regex ever stops matching, every check below still
    # "passes" -- against nothing. This is the guard on the guard.
    module = _load()
    assert len(module._INPUT_SCHEMAS) > 50
    assert "SetCameraSyncMode" in module._INPUT_SCHEMAS


@pytest.mark.parametrize("schema,params,expected", [
    # The two real 2026-08-13 failures, with the schemas as they were.
    ({"type": "object",
      "properties": {"intervalMs": {"type": "integer", "minimum": 5, "maximum": 5000}},
      "additionalProperties": False},
     {"intervalMs": 0},
     "$.intervalMs: value violates minimum"),
    ({"type": "object",
      "properties": {"mode": {"type": "string", "enum": ["off", "legacy"]}},
      "additionalProperties": False},
     {"mode": "wakepredict"},
     "$.mode: value is not in enum"),
    # additionalProperties:false is what turns a stray field into a total
    # failure rather than an ignored one.
    ({"type": "object", "properties": {"a": {"type": "integer"}},
      "additionalProperties": False},
     {"a": 1, "b": 2},
     "$.b: additional property is not allowed"),
    ({"type": "object", "properties": {"a": {"type": "integer"}},
      "required": ["a"], "additionalProperties": False},
     {},
     "$.a: required property is missing"),
    ({"type": "object", "properties": {"a": {"type": "integer", "maximum": 10}},
      "additionalProperties": False},
     {"a": 99},
     "$.a: value violates maximum"),
    ({"type": "object", "properties": {"a": {"type": "string"}},
      "additionalProperties": False},
     {"a": 5},
     "$.a: value is not a string"),
])
def test_rejects_what_the_dispatcher_rejects(schema, params, expected):
    module = _load()
    assert module._schema_violation(schema, params) == expected


@pytest.mark.parametrize("schema,params", [
    ({"type": "object", "properties": {"a": {"type": "integer", "minimum": 0}},
      "additionalProperties": False}, {"a": 0}),
    ({"type": "object", "properties": {"m": {"type": "string", "enum": ["x", "y"]}},
      "additionalProperties": False}, {"m": "y"}),
    ({"type": "object", "properties": {"f": {"type": "number"}},
      "additionalProperties": False}, {"f": 1}),      # an int satisfies "number"
])
def test_accepts_valid_parameters(schema, params):
    # ⚠️ THE FALSE-FAILURE SIDE MATTERS AS MUCH. A validator that rejects valid
    # calls would make every probe unrunnable offline and get switched off.
    module = _load()
    assert module._schema_violation(schema, params) is None


def test_an_unknown_command_is_not_validated():
    # Commands the regex cannot find must pass through silently. Inventing a
    # failure for them would block probes that call something this parser simply
    # does not understand.
    module = _load()
    assert module._validate_input("Tapioca.NoSuchCommandAnywhere", {"whatever": 1}) is None


def test_the_live_camera_sync_schemas_accept_what_the_matrix_sends():
    # The exact calls the CameraSyncMatrix probe makes, against the schemas
    # currently in the tree. This is the regression that would have caught all
    # three lost runs.
    module = _load()
    assert module._validate_input(
        "Tapioca.SetCameraSyncMode", {"mode": "wakepredict", "intervalMs": 15}) is None
    assert module._validate_input(
        "Tapioca.ViewerNavLog", {"enable": True, "intervalMs": 0, "sampler": False}) is None
