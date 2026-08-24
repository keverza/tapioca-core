"""Shared schema-form command inputs."""

import importlib
import os
import sys

import pytest

pytest.importorskip("pydantic", reason="pydantic ships in the Tapioca runtime baseline")

_PACKAGE = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

_ports = importlib.import_module("evp._ports")
_inputs = importlib.import_module("evp.inputs")
_public_inputs = importlib.import_module("tapioca.inputs")
NodeModel = importlib.import_module("evp.schema").NodeModel

FileIOInputs = _inputs.FileIOInputs
read_file = _inputs.read_file
save_file = _inputs.save_file


def test_public_library_alias_and_version():
    assert _inputs.INPUTS_VERSION == "1.0.0"
    assert _public_inputs.INPUTS_VERSION == _inputs.INPUTS_VERSION
    assert _public_inputs.read_file is _inputs.read_file


def _by_name(model):
    return {param["name"]: param for param in _ports.ports_from_schema(model.model_json_schema())}


def test_file_fields_emit_native_dialog_metadata():
    class Inputs(NodeModel):
        source: str = read_file("csv", "xlsx", title="Source")
        report: str = save_file("json", "csv", title="Report")

    params = _by_name(Inputs)

    assert params["source"]["type"] == "FilePath"
    assert params["source"]["mode"] == "open"
    assert params["source"]["extensions"] == ["csv", "xlsx"]
    assert params["report"]["mode"] == "save"
    assert params["report"]["extensions"] == ["json", "csv"]


def test_file_io_group_is_composable():
    class Inputs(FileIOInputs):
        dry_run: bool = False

    params = _by_name(Inputs)

    assert params["input_path"]["mode"] == "open"
    assert params["output_path"]["mode"] == "save"
    assert params["dry_run"]["type"] == "Bool"
