"""Opt-in, bounded composition measurements; no benchmark framework or host access."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import time
import tomllib
from pathlib import Path

LANES = ("cpp", "abi", "facade")
COUNTS = (0, 1, 16, 256, 4096)
SEED = 0x5EED2026
ROOT = Path(__file__).resolve().parents[1]


def bounded_integer(text: str, maximum: int) -> int:
    value = int(text)
    if not 1 <= value <= maximum:
        raise argparse.ArgumentTypeError(f"expected 1..{maximum}")
    return value


def validate(record: dict, lane: str, count: int, iterations: int) -> None:
    """Refuse partial/mislabelled/non-finite results, not merely successful exits."""
    exact = {
        "lane": lane,
        "count": count,
        "iterations": iterations,
        "seed": SEED,
        "workerCount": 1,
        "optimized": True,
        "parityAbsoluteTolerance": 1e-12,
    }
    for key, value in exact.items():
        if type(record.get(key)) is not type(value) or record[key] != value:
            raise ValueError(f"unexpected benchmark field {key}")
    if record.get("configuration") not in ("Release", "RelWithDebInfo"):
        raise ValueError("optimized CMake Release/RelWithDebInfo build required")
    if not record.get("compiler") or type(record.get("compilerVersion")) is not int:
        raise ValueError("missing compiler metadata")
    fixture_hash = record.get("fixtureHash", "")
    if not isinstance(fixture_hash, str) or not fixture_hash.isascii() or not fixture_hash.isdecimal():
        raise ValueError("invalid fixture hash")
    if not 0 <= int(fixture_hash) < 2**64:
        raise ValueError("fixture hash overflows u64")
    for key in ("nanosecondsPerCall", "checksum"):
        value = record.get(key)
        if type(value) not in (float, int) or not math.isfinite(value):
            raise ValueError(f"invalid measurement {key}")
    if record["nanosecondsPerCall"] <= 0:
        raise ValueError("nonpositive elapsed time")
    calls = iterations if lane == "facade" and count else 0
    if type(record.get("cppNewCalls")) is not int or record["cppNewCalls"] != calls:
        raise ValueError("ordinary C++ new probe mismatch: cppNewCalls")
    requested = record.get("cppNewRequestedBytes")
    if (
        type(requested) is not int
        or not calls * count * 96 <= requested < 2**64
        or (calls == 0 and requested != 0)
        or requested % iterations
    ):
        raise ValueError("ordinary C++ new probe mismatch: cppNewRequestedBytes")
    before, after = record.get("peakRssBeforeBytes"), record.get("peakRssAfterBytes")
    if type(before) is not int or type(after) is not int or not 0 < before <= after:
        raise ValueError("invalid process-lifetime peak RSS")


def collect(executable: Path, samples: int, iterations: int) -> list[dict]:
    records = []
    hashes = {}
    build_metadata = None
    for count in COUNTS:
        for sample in range(samples):
            # Rotate first lane to avoid giving one backend every cold-first slot.
            for lane in LANES[sample % 3 :] + LANES[: sample % 3]:
                completed = subprocess.run(
                    [str(executable), lane, str(count), str(iterations)],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=120,
                )
                record = json.loads(completed.stdout)
                validate(record, lane, count, iterations)
                signature = tuple(record[k] for k in ("compiler", "compilerVersion", "configuration"))
                if build_metadata is not None and build_metadata != signature:
                    raise ValueError("compiler/configuration changed during run")
                build_metadata = signature
                prior_hash = hashes.setdefault(count, record["fixtureHash"])
                if record["fixtureHash"] != prior_hash:
                    raise ValueError("backends/samples did not use identical fixtures")
                record["sample"] = sample
                records.append(record)
    return records


def summarize(records: list[dict]) -> list[dict]:
    summary = []
    for count in COUNTS:
        for lane in LANES:
            group = [r for r in records if r["count"] == count and r["lane"] == lane]
            times = [r["nanosecondsPerCall"] for r in group]
            summary.append(
                {
                    "count": count,
                    "lane": lane,
                    "samples": len(group),
                    "nanosecondsPerCallMedian": statistics.median(times),
                    "nanosecondsPerCallMin": min(times),
                    "nanosecondsPerCallMax": max(times),
                    "cppNewCallsPerCall": group[0]["cppNewCalls"] // group[0]["iterations"],
                    "cppNewRequestedBytesPerCall": group[0]["cppNewRequestedBytes"] // group[0]["iterations"],
                    "processPeakRssBytesMax": max(r["peakRssAfterBytes"] for r in group),
                    "abiIngressCopyBytesPerCallSourceModel": count * 96 if lane == "facade" else 0,
                }
            )
    return summary


def build_settings(cache: Path) -> dict[str, str]:
    keys = {
        "CMAKE_CXX_FLAGS",
        "CMAKE_CXX_FLAGS_RELEASE",
        "CMAKE_CXX_FLAGS_RELWITHDEBINFO",
        "CMAKE_GENERATOR",
        "CMAKE_GENERATOR_PLATFORM",
        "CMAKE_GENERATOR_TOOLSET",
        "CMAKE_MSVC_RUNTIME_LIBRARY",
        "TAPIOCA_LINK_RUST_GEOMETRY",
        "TAPIOCA_BUILD_GEOMETRY_BENCHMARKS",
        "EVP_SANITIZE",
    }
    values = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if "=" in line and ":" in line.split("=", 1)[0]:
            key = line.split(":", 1)[0]
            if key in keys:
                values[key] = line.split("=", 1)[1]
    for key, expected in {
        "TAPIOCA_LINK_RUST_GEOMETRY": "ON",
        "TAPIOCA_BUILD_GEOMETRY_BENCHMARKS": "ON",
        "EVP_SANITIZE": "OFF",
    }.items():
        if values.get(key) != expected:
            raise ValueError(f"benchmark CMake cache requires {key}={expected}")
    flags = " ".join(v for k, v in values.items() if k.startswith("CMAKE_CXX_FLAGS")).casefold()
    if any(flag in flags for flag in ("/fp:fast", "-ffast-math", "-ofast")):
        raise ValueError("fast-math is not an accepted geometry benchmark configuration")
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cmake-cache", type=Path, required=True)
    parser.add_argument("--samples", type=lambda s: bounded_integer(s, 31), default=9)
    parser.add_argument("--iterations", type=lambda s: bounded_integer(s, 10000), default=256)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    settings = build_settings(args.cmake_cache)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    before_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    cargo_home = Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo"))
    rustup = shutil.which("rustup") or str(cargo_home / "bin" / ("rustup.exe" if os.name == "nt" else "rustup"))
    pin = tomllib.loads((ROOT / "AddOn/EvP/rust/rust-toolchain.toml").read_text())["toolchain"]["channel"]
    rustc = subprocess.check_output([rustup, "run", pin, "rustc", "-vV"], text=True, timeout=30)
    records = collect(executable, args.samples, args.iterations)
    if hashlib.sha256(executable.read_bytes()).hexdigest() != before_hash:
        raise ValueError("benchmark executable changed during measurement")
    report = {
        "schemaVersion": 1,
        "coreCommit": commit,
        "dirtyStatus": dirty.splitlines(),
        "executable": str(executable),
        "executableSha256": before_hash,
        "cmakeCache": str(args.cmake_cache.resolve()),
        "cmakeSettings": settings,
        "recordedAtUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "logicalCpuCount": os.cpu_count(),
        "rustcVersionVerbose": rustc.splitlines(),
        "rustProfile": "release (CMake RustKernel policy)",
        "workerCount": 1,
        "cpuAffinityPinned": False,
        "samples": args.samples,
        "iterations": args.iterations,
        "scope": "Compiled C++ fold vs Rust raw ABI vs owned facade on identical finite near-identity chains.",
        "allocationScope": (
            "Calling-thread ordinary C++ new/new[] calls and requested bytes only; "
            "NOT Rust/malloc/aligned/system allocations or retained heap."
        ),
        "copyScope": (
            "Source-derived explicit ABI input copy volume only, "
            "NOT measured memory traffic or all matrix/result copies."
        ),
        "memoryScope": (
            "OS process-lifetime high-water RSS including startup, both fixture representations, "
            "all-lane parity preflight and selected-lane warmup; NOT per-operation peak live heap. "
            "One fresh process per lane/count/sample."
        ),
        "timingScope": (
            "8 warmups then steady-clock batch average; includes dispatch, validation/ownership "
            "and enabled C++ new probe. C++ oracle does not have Rust ingress/refusal checks. "
            "No CPU isolation/power-policy control or production latency guarantee."
        ),
        "classification": (
            "Measurement only; composition remains worker-classified. "
            "Full allocation/copy/peak-heap and broader primitive D3 budgets remain open."
        ),
        "records": records,
        "summary": summarize(records),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{len(records)} fresh-process samples; report: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
