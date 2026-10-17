"""Run opt-in tests against a running Archicad 29 without opening or changing projects."""

from __future__ import annotations

import argparse
import math
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def positive_number(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("smoke", "massing", "surface"), default="smoke")
    parser.add_argument("--port", type=int, choices=range(19723, 19744), metavar="19723..19743")
    parser.add_argument("--api-timeout", type=positive_number, default=3.0)
    parser.add_argument("--workflow-timeout", type=positive_number, default=30.0)
    parser.add_argument(
        "--tapioca-port",
        type=int,
        choices=range(1, 65536),
        metavar="PORT",
        help="Execute Tapioca test commands over its HTTP bus instead of Archicad JSON",
    )
    parser.add_argument("--expected-project", type=Path, help="Exact disposable PLN path; required for numeric suites")
    parser.add_argument("--fixture", type=Path, help="Explicit GUIDs and independently known expected metrics JSON")
    parser.add_argument("--connect-only", action="store_true", help="Phase 1: report missing bridge without failing")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "test-results")
    parser.add_argument("--log", type=Path, action="append", default=[], help="Preserve last 64 KiB of an explicit log")
    options = parser.parse_args(argv)
    if options.suite != "smoke" and (not options.expected_project or not options.fixture or options.connect_only):
        parser.error("numeric suites require --expected-project and --fixture, and do not allow --connect-only")
    if options.suite == "surface" and options.tapioca_port is None:
        parser.error("surface requires --tapioca-port (native fragment commands use Tapioca HTTP)")
    try:
        import pytest

        from tools.test_report import LiveReport
    except ImportError as exc:
        print(f"Missing live-test dependency: {exc}. Install tools/live-requirements.txt", file=sys.stderr)
        return 2
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory = options.results_dir.resolve() / stamp
    directory.mkdir(parents=True)
    report = LiveReport(options, directory)
    tests = [str(ROOT / "AddOn/EvP/tests/live/test_smoke.py")]
    if options.suite == "massing":
        tests.append(str(ROOT / "AddOn/EvP/tests/live/test_massing.py"))
    elif options.suite == "surface":
        tests.append(str(ROOT / "AddOn/EvP/tests/live/test_surface.py"))
    code = 2
    try:
        code = pytest.main(
            ["-q", "-s", "-ra", "--tb=short", *tests, f"--junitxml={directory / 'junit.xml'}"], plugins=[report]
        )
    finally:
        report.write(code)
        print(f"Live report: {directory / 'report.json'}")
    return int(code)


if __name__ == "__main__":
    raise SystemExit(main())
