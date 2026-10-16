"""pytest reporting for the opt-in live runner; no application control."""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from tools.archicad_client import ArchicadClient


def json_value(value):
    if hasattr(value, "to_dict"):
        return value.to_dict()
    return repr(value)


class LiveReport:
    def __init__(self, options, directory: Path):
        self.options = options
        self.directory = directory
        self.client = ArchicadClient(
            port=options.port, timeout=options.api_timeout, tapioca_port=getattr(options, "tapioca_port", None)
        )
        self.tests = []
        self.errors = []

    def pytest_configure(self, config):
        config._tapioca_live_runner = self

    @pytest.hookimpl(hookwrapper=True)
    def pytest_runtest_makereport(self, item, call):
        outcome = yield
        report = outcome.get_result()
        if call.excinfo is not None:
            error = call.excinfo.value
            category = getattr(error, "category", None)
            if category is None:
                category = "assertion_failure" if isinstance(error, AssertionError) else "test_error"
            report.user_properties.append(("failureCategory", category))

    def pytest_runtest_logreport(self, report):
        self.tests.append(
            {
                "test": report.nodeid,
                "phase": report.when,
                "outcome": report.outcome,
                "durationSeconds": report.duration,
                "failure": str(report.longrepr) if report.failed else None,
                "failureCategory": dict(report.user_properties).get("failureCategory"),
            }
        )

    def write(self, exit_code: int):
        if self.options.fixture:
            try:
                (self.directory / "fixture.json").write_text(
                    self.options.fixture.read_text(encoding="utf-8"), encoding="utf-8"
                )
            except OSError as exc:
                self.errors.append(f"Cannot preserve fixture: {exc}")
        payload = {
            "exitCode": int(exit_code),
            "health": self.client.health,
            "tests": self.tests,
            "trace": self.client.trace,
            "diagnosticErrors": self.errors,
            "configuration": {
                "port": self.options.port,
                "apiTimeout": self.options.api_timeout,
                "tapiocaPort": self.client.tapioca_port,
                "expectedProject": str(self.options.expected_project) if self.options.expected_project else None,
            },
            "limitations": [
                "No process ownership established: disconnection is not proof of an application crash.",
                "Screenshots/overlay capture are unsupported in this MVP.",
                "No project open/save, model mutation, build, binary replacement or restart is performed.",
            ],
        }
        for index, source in enumerate(self.options.log):
            try:
                with Path(source).open("rb") as stream:
                    stream.seek(0, os.SEEK_END)
                    stream.seek(max(0, stream.tell() - 65536))
                    excerpt = stream.read().decode("utf-8", errors="replace")
                (self.directory / f"log-{index}.txt").write_text(excerpt, encoding="utf-8")
            except OSError as exc:
                self.errors.append(f"Cannot read log {source}: {exc}")
        temporary = self.directory / "report.json.tmp"
        temporary.write_text(json.dumps(payload, indent=2, default=json_value), encoding="utf-8")
        temporary.replace(self.directory / "report.json")
