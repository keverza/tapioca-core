"""Test a disposable PLN in an owned Archicad, then quit cleanly without saving."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.archicad_client import ArchicadClient, LiveTestError  # noqa: E402
from tools.live_test import main as run_tests  # noqa: E402
from tools.live_test import positive_number  # noqa: E402
from tools.surface_fixture import validate_fixture as validate_surface_fixture  # noqa: E402
from tools.test_report import json_value  # noqa: E402


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def listeners(port: int) -> set[int | None]:
    import psutil

    return {
        item.pid
        for item in psutil.net_connections(kind="tcp")
        if item.status == psutil.CONN_LISTEN and item.laddr.port == port
    }


def require_idle(ports):
    import psutil

    running = [
        p.info["pid"]
        for p in psutil.process_iter(["pid", "name"])
        if (p.info["name"] or "").casefold() == "archicad.exe"
    ]
    if running:
        raise LiveTestError("unsafe_instance", f"Archicad is already running (PIDs {running}); no instance was changed")
    for port in ports:
        if listeners(port):
            raise LiveTestError("port_in_use", f"Port {port} is already occupied; refusing to adopt its service")


class LiveSession:
    def __init__(self, options, directory: Path):
        self.options = options
        self.directory = directory
        self.project = getattr(options, "project_copy", None) or options.project
        self.process = None
        self.process_birth = None
        self.client = ArchicadClient(port=options.port, timeout=options.api_timeout)
        self.http = ArchicadClient(port=options.port, timeout=options.api_timeout, tapioca_port=options.tapioca_port)
        self.report = {
            "exitCode": 1,
            "stages": [],
            "errors": [],
            "trace": self.client.trace,
            "httpTrace": self.http.trace,
            "sourceProject": str(options.project),
            "disposableProject": str(self.project),
            "projectCopyRequested": self.project != options.project,
            "savedProject": False,
            "forcedTermination": False,
        }

    def stage(self, name, operation):
        row = {"name": name, "outcome": "running"}
        self.report["stages"].append(row)
        started = time.monotonic()
        print(f"[live session] {name}", flush=True)
        try:
            result = operation()
            row["outcome"] = "passed"
            row["result"] = result
            return result
        except BaseException as exc:
            row["outcome"] = "failed"
            row["error"] = str(exc)
            raise
        finally:
            row["elapsedSeconds"] = time.monotonic() - started

    def require_owned(self, port):
        import psutil

        if self.process is None or self.process.poll() is not None:
            code = self.process.returncode if self.process else None
            raise LiveTestError("process_exited", f"Owned Archicad exited unexpectedly (code {code})")
        process = psutil.Process(self.process.pid)
        if process.create_time() != self.process_birth or Path(process.exe()).resolve() != self.options.archicad_exe:
            raise LiveTestError("unsafe_instance", "Process identity no longer matches the launched Archicad")
        if listeners(port) != {self.process.pid}:
            raise LiveTestError("unsafe_instance", f"Port {port} does not belong exclusively to PID {self.process.pid}")

    def guard_project(self):
        self.require_owned(self.options.port)
        return self.client.verify_project(self.project)

    def launch(self):
        require_idle((self.options.port, self.options.tapioca_port))
        if self.project != self.options.project:
            # The caller explicitly names one reusable disposable copy. Never
            # replace it while any Archicad is open, or alias the protected input.
            if self.project.exists() and os.path.samefile(self.project, self.options.project):
                raise LiveTestError("unsafe_project", "Disposable copy aliases the source PLN")
            self.project.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(self.options.project, self.project)
        import psutil

        self.process = subprocess.Popen(
            [str(self.options.archicad_exe), str(self.project)], cwd=self.options.archicad_exe.parent
        )
        self.process_birth = psutil.Process(self.process.pid).create_time()
        self.report["pid"] = self.process.pid
        self.report["commandLine"] = [str(self.options.archicad_exe), str(self.project)]
        return {"pid": self.process.pid}

    def wait_ready(self):
        deadline = time.monotonic() + self.options.startup_timeout
        last_error = "JSON API has not started"
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise LiveTestError("process_exited", f"Archicad exited during startup: {self.process.returncode}")
            if listeners(self.options.port):
                self.require_owned(self.options.port)
                try:
                    self.client.connect()
                    project = self.guard_project()
                    return {"project": project, "archicad": self.client.health}
                except LiveTestError as exc:
                    if exc.category not in {"api_timeout", "api_disconnected", "command_error", "unsafe_project"}:
                        raise
                    last_error = str(exc)
            time.sleep(0.5)
        raise LiveTestError("startup_timeout", f"No verified test project within deadline: {last_error}")

    def run_tapir(self, fixture):
        self.guard_project()
        for name in ("GetDetailsOfElements", "QuitArchicad"):
            if not self.client.available("TapirCommand", name):
                raise LiveTestError("capability_missing", f"TapirCommand.{name} is unavailable")
        surface = getattr(self.options, "suite", "massing") == "surface"
        guids = fixture["elementTypes"] if surface else fixture["elementGuids"]
        types = list(fixture["elementTypes"].values()) if surface else ["Slab"] * len(guids)
        elements = [{"elementId": {"guid": guid}} for guid in guids]
        result = self.client.addon("TapirCommand", "GetDetailsOfElements", {"elements": elements})
        records = result.get("detailsOfElements", [])
        if len(records) != len(elements) or any(
            row.get("type") != kind for row, kind in zip(records, types, strict=True)
        ):
            raise LiveTestError("fixture_error", f"Saved PLN lacks the expected element GUIDs/types: {result}")
        (self.directory / "tapir-elements.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        return {"elementCount" if surface else "slabCount": len(records)}

    def start_server(self):
        self.guard_project()
        if not self.client.available("Tapioca", "StartServer"):
            raise LiveTestError("capability_missing", "Install the build exposing Tapioca.StartServer on Archicad JSON")
        result = self.client.addon("Tapioca", "StartServer")
        if result.get("started") is not True or result.get("port") != self.options.tapioca_port:
            raise LiveTestError("server_start_failed", f"Unexpected server start response: {result}")
        self.require_owned(self.options.tapioca_port)
        health = self.http.server_health()
        status = self.http.tapioca_call("Tapioca.GetStatus")
        if health.get("ok") is not True or health.get("acVersion") != 29 or not status.get("modelOpen"):
            raise LiveTestError("protocol_error", f"Unexpected Tapioca server state: {health}, {status}")
        if status.get("jsonPort") != self.options.port or status.get("port") != self.options.tapioca_port:
            raise LiveTestError("unsafe_instance", f"HTTP service is attached to different ports: {status}")
        return {"start": result, "health": health, "status": status}

    def run_suite(self):
        self.guard_project()
        self.require_owned(self.options.tapioca_port)
        code = run_tests(
            [
                "--suite",
                getattr(self.options, "suite", "massing"),
                "--port",
                str(self.options.port),
                "--tapioca-port",
                str(self.options.tapioca_port),
                "--expected-project",
                str(self.project),
                "--fixture",
                str(self.directory / "fixture.json"),
                "--results-dir",
                str(self.directory / "tests"),
                "--api-timeout",
                str(self.options.api_timeout),
                "--workflow-timeout",
                str(self.options.workflow_timeout),
            ]
        )
        self.report["pytestExitCode"] = code
        if code != 0:
            raise LiveTestError("tests_failed", f"Live pytest returned {code}; see the nested tests report")
        return {"pytestExitCode": code, "transport": "Tapioca HTTP /evp/call"}

    def quit(self):
        self.guard_project()
        try:
            response = self.client.addon("TapirCommand", "QuitArchicad")
            if response.get("success") is not True:
                raise LiveTestError("shutdown_failed", f"Tapir did not acknowledge quit: {response}")
        except LiveTestError as exc:
            if exc.category not in {"api_disconnected", "api_timeout"}:
                raise
            self.report["quitTransportWarning"] = str(exc)
        try:
            code = self.process.wait(timeout=self.options.shutdown_timeout)
        except subprocess.TimeoutExpired as exc:
            raise LiveTestError("shutdown_timeout", "Owned Archicad did not exit; no force-kill was attempted") from exc
        self.report["processExitCode"] = code
        if code != 0:
            raise LiveTestError("process_crash", f"Archicad exited with nonzero code {code}")
        for port in (self.options.port, self.options.tapioca_port):
            if listeners(port):
                raise LiveTestError("shutdown_failed", f"Port {port} still has a listener after shutdown")
        return {"processExitCode": code, "jsonPortClosed": True, "tapiocaPortClosed": True}

    def run(self):
        source_hash = None
        try:
            source_hash = digest(self.options.project)
            self.report["sourceSha256Before"] = source_hash
            fixture = json.loads(self.options.fixture.read_text(encoding="utf-8"))
            if getattr(self.options, "suite", "massing") == "surface":
                validate_surface_fixture(fixture)
            elif not 1 <= len(fixture["elementGuids"]) <= 8:
                raise LiveTestError("fixture_error", "Expected 1..8 slab GUIDs")
            shutil.copy2(self.options.fixture, self.directory / "fixture.json")
            self.stage("launch_archicad", self.launch)
            self.stage("wait_for_owned_project", self.wait_ready)
            self.stage("tapir_read_fixture", lambda: self.run_tapir(fixture))
            self.stage("start_tapioca_server", self.start_server)
            self.stage(f"tapioca_http_{getattr(self.options, 'suite', 'massing')}_tests", self.run_suite)
            self.report["exitCode"] = 0
        except (Exception, KeyboardInterrupt) as exc:
            self.report["errors"].append({"category": getattr(exc, "category", "session_error"), "message": str(exc)})
        finally:
            if self.process is not None:
                try:
                    self.stage("quit_owned_archicad", self.quit)
                except (Exception, KeyboardInterrupt) as exc:
                    self.report["exitCode"] = 1
                    self.report["errors"].append(
                        {"category": getattr(exc, "category", "cleanup_error"), "message": str(exc)}
                    )
                    self.report["manualCleanupMayBeRequired"] = self.process.poll() is None
            try:
                after_hash = digest(self.options.project)
                self.report["sourceSha256After"] = after_hash
                self.report["sourceUnchanged"] = source_hash is not None and after_hash == source_hash
                if not self.report["sourceUnchanged"]:
                    raise LiveTestError("source_changed", "Master PLN changed during run")
            except (OSError, LiveTestError) as exc:
                self.report["exitCode"] = 1
                self.report["errors"].append({"category": "source_changed", "message": str(exc)})
            self.capture_logs()
            temporary = self.directory / "session.json.tmp"
            temporary.write_text(json.dumps(self.report, indent=2, default=json_value), encoding="utf-8")
            temporary.replace(self.directory / "session.json")
        return self.report["exitCode"]

    def capture_logs(self):
        paths = list(self.options.log)
        log_root = Path(os.environ.get("LOCALAPPDATA", "")) / "Tapioca/logs"
        paths.extend(path for name in ("startup.log", "archviz.log") if (path := log_root / name).is_file())
        for index, path in enumerate(paths):
            try:
                with path.open("rb") as stream:
                    stream.seek(0, os.SEEK_END)
                    stream.seek(max(0, stream.tell() - 65536))
                    (self.directory / f"native-log-{index}.txt").write_bytes(stream.read())
            except OSError as exc:
                self.report.setdefault("logErrors", []).append(str(exc))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--project", type=Path, required=True, help="Saved disposable PLN; opened directly, never saved"
    )
    parser.add_argument(
        "--project-copy",
        type=Path,
        help="Optional single reusable disposable PLN path; refreshed from --project only while Archicad is closed",
    )
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--suite", choices=("massing", "surface"), default="massing")
    parser.add_argument(
        "--archicad-exe",
        type=Path,
        default=Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "GRAPHISOFT/Archicad 29/ARCHICAD.exe",
    )
    parser.add_argument("--port", type=int, choices=range(19723, 19744), default=19723, metavar="PORT")
    parser.add_argument("--tapioca-port", type=int, choices=(19191,), default=19191)
    parser.add_argument("--startup-timeout", type=positive_number, default=180.0)
    parser.add_argument("--shutdown-timeout", type=positive_number, default=60.0)
    parser.add_argument("--api-timeout", type=positive_number, default=3.0)
    parser.add_argument("--workflow-timeout", type=positive_number, default=30.0)
    parser.add_argument("--results-dir", type=Path, default=ROOT / "test-results")
    parser.add_argument("--log", type=Path, action="append", default=[])
    options = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("Owned launch/shutdown currently supports Windows only")
    for name in ("project", "fixture", "archicad_exe"):
        value = getattr(options, name).resolve()
        if not value.is_file():
            parser.error(f"{name} is not a file: {value}")
        setattr(options, name, value)
    if options.project.suffix.casefold() != ".pln" or options.archicad_exe.name.casefold() != "archicad.exe":
        parser.error("Expected a .pln project and ARCHICAD.exe")
    if options.project_copy:
        options.project_copy = options.project_copy.resolve()
        if options.project_copy == options.project or options.project_copy.suffix.casefold() != ".pln":
            parser.error("--project-copy must name a different disposable .pln file")
    try:
        import psutil  # noqa: F401
    except ImportError:
        parser.error("Install tools/live-requirements.txt (psutil is required for process/port ownership)")
    directory = options.results_dir.resolve() / datetime.now(timezone.utc).strftime("session-%Y%m%dT%H%M%S.%fZ")
    directory.mkdir(parents=True)
    session = LiveSession(options, directory)
    code = session.run()
    print(f"Session report: {directory / 'session.json'}")
    for error in session.report["errors"]:
        print(f"{error['category']}: {error['message']}", file=sys.stderr)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
