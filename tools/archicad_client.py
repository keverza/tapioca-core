"""Small, read-only live adapter. The official archicad package owns the wire protocol."""

from __future__ import annotations

import contextlib
import json
import socket
import time
from pathlib import Path
from typing import Any
from urllib.error import URLError
from urllib.parse import quote
from urllib.request import Request, urlopen
from uuid import UUID


class LiveTestError(RuntimeError):
    def __init__(self, category: str, message: str):
        self.category = category
        super().__init__(f"{category}: {message}")


def normalized_guid(value: str) -> str:
    return str(UUID(value.strip("{}")))


class ArchicadClient:
    def __init__(self, *, port: int | None = None, timeout: float = 3.0, tapioca_port: int | None = None):
        self.port = port
        self.timeout = timeout
        self.tapioca_port = tapioca_port
        self.connection = None
        self.trace: list[dict[str, Any]] = []
        self.health: dict[str, Any] = {}

    @contextlib.contextmanager
    def _bounded_socket(self):
        # archicad 29.3000 uses urllib without a timeout argument. This runner is
        # single-threaded and restores the process default after each SDK call.
        previous = socket.getdefaulttimeout()
        socket.setdefaulttimeout(self.timeout)
        try:
            yield
        finally:
            socket.setdefaulttimeout(previous)

    def _call(self, name, function, *args):
        record = {"command": name, "arguments": args, "started": time.time()}
        self.trace.append(record)
        start = time.monotonic()
        try:
            with self._bounded_socket():
                result = function(*args)
            record["response"] = result
            return result
        except (TimeoutError, URLError, ConnectionError, OSError) as exc:
            category = "api_timeout" if isinstance(exc, TimeoutError) else "api_disconnected"
            if isinstance(getattr(exc, "reason", None), TimeoutError):
                category = "api_timeout"
            record["error"] = repr(exc)
            raise LiveTestError(category, str(exc)) from exc
        except Exception as exc:
            record["error"] = repr(exc)
            raise LiveTestError("command_error", f"{name}: {exc}") from exc
        finally:
            record["elapsedMs"] = (time.monotonic() - start) * 1000

    def connect(self):
        try:
            from archicad import ACConnection
        except ImportError as exc:
            raise LiveTestError("dependency_missing", "Install tools/live-requirements.txt") from exc
        ports = [self.port] if self.port is not None else range(19723, 19744)
        found = []
        for port in ports:
            try:
                connection = self._call(f"connect:{port}", ACConnection, port)
            except LiveTestError as exc:
                if exc.category in {"api_disconnected", "api_timeout"}:
                    if self.port is not None:
                        raise LiveTestError(
                            exc.category,
                            f"Cannot reach Archicad JSON API on {self.port}; start the dedicated AC29 instance. {exc}",
                        ) from exc
                    continue
                raise
            if connection is not None:
                found.append(connection)
        if not found:
            raise LiveTestError("api_disconnected", "No Archicad JSON API found; start a dedicated AC29 instance")
        if len(found) != 1:
            raise LiveTestError("ambiguous_instance", "Multiple Archicad instances found; specify --port")
        self.connection = found[0]
        self.port = self.connection.port
        self.health.update(
            port=self.port,
            archicadVersion=self.connection.version,
            archicadBuild=self.connection.build,
            language=self.connection.lang,
        )
        if self.connection.version != 29:
            raise LiveTestError("wrong_version", f"Requires Archicad 29, found {self.connection.version}")
        if not self._call("API.IsAlive", self.connection.commands.IsAlive):
            raise LiveTestError("api_disconnected", "API.IsAlive returned false")
        return self

    def available(self, namespace: str, command: str) -> bool:
        command_id = self.connection.types.AddOnCommandId(namespace, command)
        return self._call("API.IsAddOnCommandAvailable", self.connection.commands.IsAddOnCommandAvailable, command_id)

    def addon(self, namespace: str, command: str, parameters: dict | None = None) -> dict:
        if namespace == "TapiocaTest" and self.tapioca_port is not None:
            return self.tapioca_call(f"Tapioca.Test{command}", parameters)
        command_id = self.connection.types.AddOnCommandId(namespace, command)
        # The SDK's AddOnCommandParameters alias is dict, not a generated wrapper.
        response = self._call(
            f"{namespace}.{command}", self.connection.commands.ExecuteAddOnCommand, command_id, parameters or {}
        )
        if not isinstance(response, dict):
            raise LiveTestError("protocol_error", f"{namespace}.{command} returned a non-object")
        if response.get("ok") is False or response.get("success") is False or "error" in response:
            raise LiveTestError("command_error", f"{namespace}.{command}: {response}")
        return response

    def tapioca_call(self, command: str, parameters: dict | None = None) -> dict:
        if self.tapioca_port is None:
            raise LiveTestError("configuration_error", "Tapioca HTTP port is not configured")
        url = f"http://127.0.0.1:{self.tapioca_port}/evp/call?command={quote(command, safe='')}"

        def send(payload):
            request = Request(url, json.dumps(payload).encode("utf-8"), {"Content-Type": "application/json"})
            with urlopen(request, timeout=self.timeout) as response:
                return json.load(response)

        try:
            envelope = self._call(command, send, parameters or {})
        finally:
            self.trace[-1]["url"] = url
            self.trace[-1]["transport"] = "tapioca_http"
        if not isinstance(envelope, dict) or not isinstance(envelope.get("ok"), bool):
            raise LiveTestError("protocol_error", "Invalid Tapioca HTTP envelope")
        if not envelope["ok"] or "error" in envelope:
            raise LiveTestError("command_error", f"{command}: {envelope.get('error')}")
        if not isinstance(envelope.get("data"), dict):
            raise LiveTestError("protocol_error", "Tapioca HTTP response has no domain data")
        return envelope["data"]

    def server_health(self) -> dict:
        if self.tapioca_port is None:
            raise LiveTestError("configuration_error", "Tapioca HTTP port is not configured")
        url = f"http://127.0.0.1:{self.tapioca_port}/health"

        def get():
            with urlopen(url, timeout=self.timeout) as response:
                return json.load(response)

        try:
            return self._call("GET /health", get)
        finally:
            self.trace[-1]["url"] = url

    def discover(self) -> dict:
        self.health["tapiocaTransport"] = "http" if self.tapioca_port is not None else "archicad_json"
        for command in ("GetAddOnVersion", "GetProjectInfo"):
            if not self.available("TapirCommand", command):
                raise LiveTestError("capability_missing", f"TapirCommand.{command} is not registered")
        self.health["tapir"] = self.addon("TapirCommand", "GetAddOnVersion")
        self.health["project"] = self.addon("TapirCommand", "GetProjectInfo")
        names = ("Ping", "RunWorkflow", "GetStatus", "GetDiagnostics", "Reset", "Capture")
        self.health["tapiocaCapabilities"] = {name: self.available("TapiocaTest", name) for name in names}
        if self.health["tapiocaCapabilities"]["Ping"]:
            self.health["tapioca"] = self.addon("TapiocaTest", "Ping")
        return self.health

    def verify_project(self, expected: Path) -> dict:
        project = self.addon("TapirCommand", "GetProjectInfo")
        self.health["project"] = project
        if project.get("isUntitled") or project.get("isTeamwork"):
            raise LiveTestError("unsafe_project", "Tests require a saved, local disposable PLN")
        actual = project.get("projectPath")
        if not actual or Path(actual).resolve() != expected.resolve():
            raise LiveTestError("unsafe_project", f"Expected {expected.resolve()}, found {actual!r}")
        return project

    def wait_for_job(self, job_id: str, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            if time.monotonic() >= deadline:
                raise LiveTestError("analysis_timeout", f"Job {job_id} did not finish within {timeout}s")
            # A final HTTP call must not overshoot the analysis deadline by a full API timeout.
            previous = self.timeout
            self.timeout = min(previous, max(0.001, deadline - time.monotonic()))
            try:
                status = self.addon("TapiocaTest", "GetStatus", {"jobId": job_id})
            finally:
                self.timeout = previous
            if status.get("jobId") != job_id:
                raise LiveTestError("protocol_error", "GetStatus returned the wrong job identifier")
            state = status.get("state")
            if state == "completed":
                if not isinstance(status.get("result"), dict):
                    raise LiveTestError("protocol_error", "Completed job has no domain result")
                return status["result"]
            if state in {"failed", "cancelled"}:
                raise LiveTestError("workflow_failed", f"Job {job_id}: {status.get('errorMessage', state)}")
            if state != "running":
                raise LiveTestError("protocol_error", f"Unknown job state {state!r}")
            time.sleep(min(0.1, max(0, deadline - time.monotonic())))
