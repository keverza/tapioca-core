"""Offline regression tests for the live runner, not evidence of a live Archicad pass."""

import importlib.util
import json
import re
import socket
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.error import URLError

import pytest

from tools.archicad_client import ArchicadClient, LiveTestError
from tools.live_test import main
from tools.test_report import LiveReport

ROOT = Path(__file__).resolve().parents[1]
MASSING_PATH = ROOT / "AddOn/EvP/tests/live/test_massing.py"
spec = importlib.util.spec_from_file_location("live_massing_assertions", MASSING_PATH)
massing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(massing)
GUID = "77ee2bcc-8b47-46de-8198-a243f487bf52"


def options(**overrides):
    defaults = dict(port=19723, api_timeout=0.1, log=[], fixture=None, expected_project=None)
    return SimpleNamespace(**(defaults | overrides))


def fake_connection(port=19723, version=29):
    return SimpleNamespace(
        port=port,
        version=version,
        build=3000,
        lang="INT",
        types=SimpleNamespace(AddOnCommandId=lambda namespace, name: {"namespace": namespace, "name": name}),
        commands=SimpleNamespace(
            IsAlive=lambda: True,
            IsAddOnCommandAvailable=lambda command: True,
            ExecuteAddOnCommand=lambda command, params: {},
        ),
    )


def install_connection(monkeypatch, factory):
    monkeypatch.setitem(sys.modules, "archicad", SimpleNamespace(ACConnection=factory))


def test_multiple_instances_require_explicit_port(monkeypatch):
    install_connection(monkeypatch, lambda port: fake_connection(port) if port in (19723, 19724) else None)
    with pytest.raises(LiveTestError, match="ambiguous_instance"):
        ArchicadClient().connect()
    assert ArchicadClient(port=19724).connect().port == 19724


def test_wrong_archicad_version_fails(monkeypatch):
    install_connection(monkeypatch, lambda port: fake_connection(port, 28))
    with pytest.raises(LiveTestError, match="wrong_version"):
        ArchicadClient(port=19723).connect()


@pytest.mark.parametrize("error,category", [(TimeoutError(), "api_timeout"), (URLError("refused"), "api_disconnected")])
def test_transport_failures_preserve_category_and_restore_timeout(monkeypatch, error, category):
    client = ArchicadClient(port=19723)
    previous = socket.getdefaulttimeout()

    def fail(port):
        assert socket.getdefaulttimeout() == client.timeout
        raise error

    install_connection(monkeypatch, fail)
    with pytest.raises(LiveTestError) as failure:
        client.connect()
    assert failure.value.category == category
    assert socket.getdefaulttimeout() == previous
    assert "error" in client.trace[-1]


@pytest.mark.parametrize("response", [{"ok": False, "error": "native failed"}, {"error": {"code": 1}}, None, []])
def test_native_failure_is_not_success(response):
    client = ArchicadClient()
    client.connection = fake_connection()
    client.connection.commands.ExecuteAddOnCommand = lambda *_: response
    with pytest.raises(LiveTestError):
        client.addon("TapiocaTest", "Ping")
    assert client.trace[-1]["response"] == response


def test_project_path_is_exact_and_teamwork_is_refused(tmp_path):
    client = ArchicadClient()
    project = {"isUntitled": False, "isTeamwork": False, "projectPath": str(tmp_path / "test.pln")}
    client.addon = lambda *_: project
    client.verify_project(tmp_path / "test.pln")
    with pytest.raises(LiveTestError, match="unsafe_project"):
        client.verify_project(tmp_path / "other.pln")
    project["isTeamwork"] = True
    with pytest.raises(LiveTestError, match="unsafe_project"):
        client.verify_project(tmp_path / "test.pln")


def test_poll_completion_and_failures(monkeypatch):
    client = ArchicadClient()
    responses = iter([{"jobId": "1", "state": "running"}, {"jobId": "1", "state": "completed", "result": {"x": 3}}])
    client.addon = lambda *_: next(responses)
    monkeypatch.setattr("tools.archicad_client.time.sleep", lambda _: None)
    assert client.wait_for_job("1", 1) == {"x": 3}
    client.addon = lambda *_: {"jobId": "1", "state": "failed", "errorMessage": "missing source"}
    with pytest.raises(LiveTestError, match="workflow_failed"):
        client.wait_for_job("1", 1)
    client.addon = lambda *_: {"jobId": "2", "state": "completed", "result": {}}
    with pytest.raises(LiveTestError, match="wrong job"):
        client.wait_for_job("1", 1)


def test_analysis_timeout_is_distinct_and_restores_api_timeout(monkeypatch):
    client = ArchicadClient(timeout=5)
    client.addon = lambda *_: {"jobId": "1", "state": "running"}
    ticks = iter([0, 0.1, 0.2, 0.3, 2])
    monkeypatch.setattr("tools.archicad_client.time.monotonic", lambda: next(ticks))
    monkeypatch.setattr("tools.archicad_client.time.sleep", lambda _: None)
    with pytest.raises(LiveTestError, match="analysis_timeout"):
        client.wait_for_job("1", 1)
    assert client.timeout == 5


def sample_result():
    rows = [dict(guid=GUID, story=i, z=3 * i, height=3, areaM2=200, volumeM3=600) for i in range(3)]
    result = dict(basis="unclipped_slab_prisms", rawAreaM2=600, rawVolumeM3=1800, floorCount=3, rows=rows)
    fixture = dict(elementGuids=[GUID], expected=dict(rawAreaM2=600, rawVolumeM3=1800, floorCount=3))
    return result, fixture


def test_real_numeric_and_guid_assertions():
    result, fixture = sample_result()
    massing.assert_metrics(result, fixture)
    result["rawAreaM2"] = 601
    with pytest.raises(AssertionError):
        massing.assert_metrics(result, fixture)
    result["rawAreaM2"] = 600
    result["rows"][0]["guid"] = "c4c01264-cc0b-4f26-bb55-ad3fbad0eb80"
    with pytest.raises(AssertionError):
        massing.assert_metrics(result, fixture)


@pytest.mark.parametrize("number", [float("nan"), float("inf"), 0, -1])
def test_nonfinite_or_invalid_metrics_fail(number):
    result, fixture = sample_result()
    result["rows"][0]["height"] = number
    with pytest.raises(AssertionError):
        massing.assert_metrics(result, fixture)


def test_report_preserves_failures_requests_and_log_tail(tmp_path):
    log = tmp_path / "native.log"
    log.write_bytes(b"x" * 70000 + b"failure detail")
    report = LiveReport(options(log=[log]), tmp_path)
    report.client.trace.append({"command": "TapiocaTest.RunWorkflow", "response": {"error": "failed"}})
    report.write(1)
    result = json.loads((tmp_path / "report.json").read_text())
    assert result["exitCode"] == 1
    assert result["trace"][0]["response"]["error"] == "failed"
    assert (tmp_path / "log-0.txt").stat().st_size == 65536
    assert (tmp_path / "log-0.txt").read_text().endswith("failure detail")
    assert not (tmp_path / "report.json.tmp").exists()


@pytest.mark.parametrize("args", [["--suite", "massing"], ["--api-timeout", "nan"], ["--port", "80"]])
def test_cli_rejects_unsafe_or_unbounded_configuration(args):
    with pytest.raises(SystemExit) as result:
        main(args)
    assert result.value.code == 2


def test_official_package_round_trip(monkeypatch):
    """Verify generated command IDs, dictionary params and response unwrapping offline."""
    pytest.importorskip("archicad")
    import archicad.commands
    import archicad.releases.ac29.b3000commands

    requests = []

    def post(_request, text):
        request = json.loads(text)
        requests.append(request)
        command = request["command"]
        result = {
            "API.GetProductInfo": {"version": 29, "buildNumber": 3000, "languageCode": "INT"},
            "API.IsAlive": {"isAlive": True},
            "API.IsAddOnCommandAvailable": {"available": True},
            "API.ExecuteAddOnCommand": {"addOnCommandResponse": {"version": "0.0.1"}},
        }[command]
        return {"succeeded": True, "result": result}

    monkeypatch.setattr(archicad.commands, "post_command", post)
    monkeypatch.setattr(archicad.releases.ac29.b3000commands, "post_command", post)
    client = ArchicadClient(port=19723).connect()
    assert client.available("TapiocaTest", "Ping")
    assert client.addon("TapiocaTest", "Ping", {"sample": 42}) == {"version": "0.0.1"}
    assert requests[-1]["parameters"] == {
        "addOnCommandId": {"commandNamespace": "TapiocaTest", "commandName": "Ping"},
        "addOnCommandParameters": {"sample": 42},
    }


def native_schema(name):
    source = (ROOT / "AddOn/EvP/Sources/AddOn/NativeCommands/TestCommands.cpp").read_text(encoding="utf-8")
    raw = re.search(rf'constexpr const char {name}\[\]\s*=\s*R"json\((.*?)\)json"', source, re.DOTALL)
    assert raw is not None
    return json.loads(raw.group(1))


def test_native_schemas_accept_numeric_contract_and_refuse_unknown_fields():
    jsonschema = pytest.importorskip("jsonschema")
    for name in ("kEmpty", "kPing", "kRun", "kStarted", "kJobInput", "kStatus", "kDiagnostics", "kReset", "kCapture"):
        jsonschema.Draft7Validator.check_schema(native_schema(name))
    request = {"workflow": "massing_feasibility", "inputs": {"elementGuids": [GUID]}, "mode": "preview", "seed": 42}
    jsonschema.validate(request, native_schema("kRun"))
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(request | {"mode": "apply"}, native_schema("kRun"))
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(request | {"selection": True}, native_schema("kRun"))
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(request | {"inputs": {"elementGuids": [GUID, GUID]}}, native_schema("kRun"))
    result, _ = sample_result()
    status = dict(jobId="1", state="completed", progress=1, elapsedMs=4, errorMessage="", result=result)
    jsonschema.validate(status, native_schema("kStatus"))
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(status | {"ok": True}, native_schema("kStatus"))
