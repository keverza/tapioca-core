"""Offline safety/transport checks. These never launch or close an application."""

import io
import json
import subprocess
import sys
from types import SimpleNamespace

import pytest

from tools import live_session
from tools.archicad_client import ArchicadClient, LiveTestError


@pytest.mark.parametrize(
    "envelope",
    [{"ok": False, "error": {"message": "bad request"}}, {"ok": True}, {"data": {}}, {"ok": 1, "data": {}}, []],
)
def test_http_never_accepts_bad_envelopes(monkeypatch, envelope):
    monkeypatch.setattr(
        "tools.archicad_client.urlopen", lambda *_args, **_kwargs: io.BytesIO(json.dumps(envelope).encode())
    )
    client = ArchicadClient(tapioca_port=19191)
    with pytest.raises(LiveTestError):
        client.tapioca_call("Tapioca.TestPing")
    assert client.trace[-1]["transport"] == "tapioca_http"
    assert client.trace[-1]["response"] == envelope


def test_http_runs_canonical_native_command_with_real_bus_shape(monkeypatch):
    sent = []

    def post(request, timeout):
        sent.append((request.full_url, json.loads(request.data), timeout))
        return io.BytesIO(json.dumps({"ok": True, "data": {"jobId": "4"}, "meta": {"zone": "external"}}).encode())

    monkeypatch.setattr("tools.archicad_client.urlopen", post)
    client = ArchicadClient(tapioca_port=19191, timeout=2)
    assert client.addon("TapiocaTest", "RunWorkflow", {"seed": 42}) == {"jobId": "4"}
    assert sent == [("http://127.0.0.1:19191/evp/call?command=Tapioca.TestRunWorkflow", {"seed": 42}, 2)]


def test_preexisting_archicad_is_refused_without_touching_it(monkeypatch):
    existing = SimpleNamespace(info={"pid": 123, "name": "ARCHICAD.exe"})
    monkeypatch.setitem(sys.modules, "psutil", SimpleNamespace(process_iter=lambda _: [existing]))
    with pytest.raises(LiveTestError, match="unsafe_instance"):
        live_session.require_idle((19723, 19191))


def test_preexisting_server_is_refused(monkeypatch):
    monkeypatch.setitem(sys.modules, "psutil", SimpleNamespace(process_iter=lambda _: []))
    monkeypatch.setattr(live_session, "listeners", lambda port: {99} if port == 19191 else set())
    with pytest.raises(LiveTestError, match="port_in_use"):
        live_session.require_idle((19723, 19191))


def make_session(tmp_path):
    source = tmp_path / "source.pln"
    source.write_bytes(b"master project")
    fixture = tmp_path / "source.json"
    fixture.write_text(json.dumps({"elementGuids": ["77ee2bcc-8b47-46de-8198-a243f487bf52"]}))
    directory = tmp_path / "run"
    directory.mkdir()
    options = SimpleNamespace(
        project=source,
        fixture=fixture,
        archicad_exe=tmp_path / "ARCHICAD.exe",
        port=19723,
        tapioca_port=19191,
        api_timeout=1,
        shutdown_timeout=2,
        startup_timeout=2,
        workflow_timeout=3,
        log=[],
    )
    return live_session.LiveSession(options, directory)


class FakeProcess:
    pid = 23
    returncode = None

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        self.returncode = 0
        return 0


def test_port_and_pid_identity_must_both_match(monkeypatch, tmp_path):
    session = make_session(tmp_path)
    session.process = FakeProcess()
    session.process_birth = 123.0
    process = SimpleNamespace(create_time=lambda: 123.0, exe=lambda: str(session.options.archicad_exe))
    monkeypatch.setitem(sys.modules, "psutil", SimpleNamespace(Process=lambda _: process))
    monkeypatch.setattr(live_session, "listeners", lambda _: {23})
    session.require_owned(19723)
    monkeypatch.setattr(live_session, "listeners", lambda _: {24})
    with pytest.raises(LiveTestError, match="unsafe_instance"):
        session.require_owned(19723)
    monkeypatch.setattr(live_session, "listeners", lambda _: {23})
    process.create_time = lambda: 124.0
    with pytest.raises(LiveTestError, match="unsafe_instance"):
        session.require_owned(19723)


@pytest.mark.parametrize("test_fails", [False, True])
def test_session_copies_master_and_always_requests_owned_quit(monkeypatch, tmp_path, test_fails):
    session = make_session(tmp_path)
    events = []

    def launch():
        session.process = FakeProcess()
        assert session.project.read_bytes() == b"master project"
        events.append("launch")

    def tests():
        events.append("tests")
        if test_fails:
            raise LiveTestError("tests_failed", "numeric mismatch")

    def quit_owned():
        events.append("quit")
        session.process.returncode = 0

    monkeypatch.setattr(session, "launch", launch)
    monkeypatch.setattr(session, "wait_ready", lambda: None)
    monkeypatch.setattr(session, "run_tapir", lambda _: None)
    monkeypatch.setattr(session, "start_server", lambda: None)
    monkeypatch.setattr(session, "run_suite", tests)
    monkeypatch.setattr(session, "quit", quit_owned)
    monkeypatch.setattr(session, "capture_logs", lambda: None)
    assert session.run() == int(test_fails)
    assert events == ["launch", "tests", "quit"]
    report = json.loads((session.directory / "session.json").read_text())
    assert report["sourceUnchanged"]
    assert report["forcedTermination"] is False and report["savedProject"] is False


def test_shutdown_requires_exit_zero_and_closed_ports(monkeypatch, tmp_path):
    session = make_session(tmp_path)
    session.process = FakeProcess()
    monkeypatch.setattr(session, "guard_project", lambda: None)
    monkeypatch.setattr(session.client, "addon", lambda *_: {"success": True})
    monkeypatch.setattr(live_session, "listeners", lambda _: set())
    assert session.quit()["processExitCode"] == 0
    session.process.wait = lambda **_: -1073741819
    with pytest.raises(LiveTestError, match="process_crash"):
        session.quit()
    session.process.wait = lambda **_: 0
    monkeypatch.setattr(live_session, "listeners", lambda _: {23})
    with pytest.raises(LiveTestError, match="shutdown_failed"):
        session.quit()


def test_shutdown_timeout_never_force_kills(monkeypatch, tmp_path):
    session = make_session(tmp_path)
    session.process = FakeProcess()
    monkeypatch.setattr(session, "guard_project", lambda: None)
    monkeypatch.setattr(session.client, "addon", lambda *_: {"success": True})

    def timeout(**_):
        raise subprocess.TimeoutExpired("ARCHICAD.exe", 1)

    session.process.wait = timeout
    with pytest.raises(LiveTestError, match="shutdown_timeout"):
        session.quit()
    assert session.process.poll() is None


def test_preflight_failure_still_writes_report_without_quitting_anything(monkeypatch, tmp_path):
    session = make_session(tmp_path)

    def refuse():
        raise LiveTestError("unsafe_instance", "already open")

    monkeypatch.setattr(session, "launch", refuse)
    monkeypatch.setattr(session, "capture_logs", lambda: None)
    assert session.run() == 1
    report = json.loads((session.directory / "session.json").read_text())
    assert report["sourceUnchanged"]
    assert [row["name"] for row in report["stages"]] == ["launch_archicad"]
    assert report["errors"][0]["category"] == "unsafe_instance"
