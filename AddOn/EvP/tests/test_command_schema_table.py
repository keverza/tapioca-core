"""Structural checks for the centralized native v2 schema table."""
import json
import os
import re
from pathlib import Path


_NATIVE = os.path.join(
    os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands"
)


def test_shared_schema_definitions_are_valid_and_strict():
    with open(os.path.join(_NATIVE, "CommandSchemas.cpp"), encoding="utf-8") as source:
        blocks = re.findall(r'R"json\((.*?)\)json"', source.read(), re.DOTALL)

    definitions = json.loads(blocks[0])
    assert set(definitions) == {"ElementId", "Element", "Elements", "Point2D", "Point3D"}
    for name in ("ElementId", "Element", "Point2D", "Point3D"):
        assert definitions[name]["additionalProperties"] is False


def test_main_thread_commands_delegate_to_central_schema_lookup():
    with open(os.path.join(_NATIVE, "CommandBase.hpp"), encoding="utf-8") as source:
        command_base = source.read()

    assert "GetNativeSchemaDefinitions ()" in command_base
    assert "GetNativeInputSchema (GetName ())" in command_base
    assert "GetNativeResponseSchema (GetName ())" in command_base


def test_catalog_initialization_runs_schema_validator_self_check():
    catalog = Path(_NATIVE).parent / "Python" / "ApiCommandCatalog.cpp"
    assert "RunSchemaValidatorSelfCheck (error)" in catalog.read_text(encoding="utf-8")


def test_registry_preserves_request_and_response_schema_failures():
    registry = Path(_NATIVE) / "CommandRegistry.cpp"
    text = registry.read_text(encoding="utf-8")

    input_validation = text.index(
        "ValidateObjectStateSchema (params, command->GetInputParametersSchema ()")
    execution = text.index("result = command->ExecuteNative (params, processControl)")
    response_validation = text.index(
        "ValidateObjectStateSchema (result.data, command->GetResponseSchema ()")

    assert input_validation < execution < response_validation
    assert text.count("NativeCommandFailureKind::SchemaValidation") == 2


def test_dispatcher_maps_native_schema_failures_consistently():
    dispatcher = Path(_NATIVE).parent / "Python" / "ApiDispatcher.cpp"
    text = dispatcher.read_text(encoding="utf-8")

    assert 'result.failureKind == geomsrv::NativeCommandFailureKind::SchemaValidation' in text
    assert text.count("MakeError (NativeFailureCode (") == 2


def test_retained_native_catalog_has_one_unique_row_per_command():
    names = []
    for path in Path(_NATIVE).glob("*Commands.cpp"):
        names.extend(re.findall(
            r'\{\s*"([A-Za-z0-9]+)"\s*,\s*&MakeRegisteredNativeCommand<',
            path.read_text(encoding="utf-8")))

    # Bump WITH the command that moves it. See the note on
    # EXPECTED_REGISTRY_COMMANDS in tools/generate_tapioca_api_v2.py: this and
    # that constant count the same rows and were both found stale on 2026-08-15.
    assert len(names) == 130
    assert len(set(names)) == 130


def test_dispatcher_local_catalog_has_nineteen_unique_verbs():
    path = Path(_NATIVE).parent / "Python" / "DispatcherVerbs.cpp"
    names = re.findall(
        r'\{\s*"([A-Za-z0-9]+)"\s*,\s*DispatcherExecutionKind::',
        path.read_text(encoding="utf-8"))

    assert len(names) == 19
    assert len(set(names)) == 19
