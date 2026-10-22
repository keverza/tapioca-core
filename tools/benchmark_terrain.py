"""Bounded S3a.3 terrain measurements; builds are explicit, no host application access."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import subprocess
import time
import tomllib
from pathlib import Path

from benchmark_composition import build_settings

ROOT = Path(__file__).resolve().parents[1]
RUST = ROOT / "AddOn/EvP/rust"
FIXTURES = (
    "wall_sides",
    "steep_curb",
    "stair_treads",
    "crossing_paths",
    "courtyard_island",
    "overlap_junction",
    "grid_100x100_500",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(record: dict, lane: str, fixture: str, iterations: int) -> None:
    """Fail closed on missing metrics, debug builds, inconsistent timing or identity."""
    for key, expected in {"lane": lane, "fixture": fixture, "iterations": iterations}.items():
        if type(record.get(key)) is not type(expected) or record[key] != expected:
            raise ValueError(f"unexpected {key}")
    digest = record.get("hash", "")
    if not isinstance(digest, str) or len(digest) != 16 or any(c not in "0123456789abcdef" for c in digest):
        raise ValueError("invalid geometry digest")
    for key in ("vertices", "triangles", "constraints"):
        if type(record.get(key)) is not int or record[key] <= 0:
            raise ValueError(f"invalid {key}")
    if fixture == "grid_100x100_500" and tuple(record[k] for k in ("vertices", "triangles", "constraints")) != (
        10000,
        19602,
        500,
    ):
        raise ValueError("target workload shape changed")
    samples = record.get("samples_ms")
    if (
        not isinstance(samples, list)
        or len(samples) != iterations
        or any(type(v) not in (int, float) or not math.isfinite(v) or v <= 0 for v in samples)
    ):
        raise ValueError("invalid elapsed samples")
    ordered = sorted(samples)
    for key, expected in {
        "median_ms": ordered[iterations // 2],
        "p95_ms": ordered[math.ceil(iterations * 0.95) - 1],
    }.items():
        if type(record.get(key)) not in (int, float) or not math.isclose(record[key], expected, rel_tol=1e-5):
            raise ValueError(f"inconsistent {key}")
    if record.get("copy_bytes", 0) is not None:
        raise ValueError("copy traffic must remain explicitly unmeasured")
    if lane == "kernel":
        for key in ("rust_alloc_calls", "rust_requested_bytes", "rust_peak_live_requested_bytes"):
            if type(record.get(key)) is not int or record[key] <= 0:
                raise ValueError(f"invalid {key}")
        if record["rust_peak_live_requested_bytes"] > record["rust_requested_bytes"]:
            raise ValueError("impossible peak requested bytes")
    else:
        if (
            record.get("configuration") not in ("Release", "RelWithDebInfo")
            or not record.get("compiler")
            or not record.get("compiler_version")
        ):
            raise ValueError("optimized compiler metadata required")
        for key in (
            "cpp_new_calls",
            "cpp_new_requested_bytes",
            "known_fill_payload_copy_bytes",
            "known_mapping_conversion_bytes",
        ):
            if type(record.get(key)) is not int or record[key] <= 0:
                raise ValueError(f"invalid {key}")
        before, after = record.get("process_peak_rss_before_bytes"), record.get("process_peak_rss_after_bytes")
        if type(before) is not int or type(after) is not int or not 0 < before <= after:
            raise ValueError("invalid process peak RSS")
        if record.get("rust_alloc_calls", 0) is not None or record.get("combined_peak_heap_bytes", 0) is not None:
            raise ValueError("facade Rust/combined heap counters are unavailable")


def run(executable: Path, lane: str, fixture: str, iterations: int) -> dict:
    arguments = [str(iterations), fixture] if lane == "kernel" else [fixture, str(iterations)]
    result = subprocess.run([str(executable), *arguments], check=True, capture_output=True, text=True, timeout=180)
    lines = result.stdout.splitlines()
    if len(lines) != 2:
        raise ValueError("expected measurement and raw-samples records")
    record, samples = map(json.loads, lines)
    if samples.get("samples_for") != fixture:
        raise ValueError("mislabelled raw samples")
    record["samples_ms"] = samples["samples_ms"]
    validate(record, lane, fixture, iterations)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--facade", type=Path, required=True)
    parser.add_argument("--cmake-cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--processes", type=int, default=3)
    args = parser.parse_args()
    if not 3 <= args.iterations <= 1000 or not 1 <= args.processes <= 9:
        parser.error("iterations must be 3..1000, processes 1..9")
    executables = {"kernel": args.kernel.resolve(strict=True), "facade": args.facade.resolve(strict=True)}
    settings = build_settings(args.cmake_cache)
    hashes = {lane: sha256(path) for lane, path in executables.items()}
    # Capture exact source/binary identity, not an unverifiable clean-tree claim.
    sources = list((RUST / "geometry/src/terrain").rglob("*.rs")) + [
        RUST / "geometry/src/terrain.rs",
        RUST / "Cargo.lock",
        RUST / "tests/fixtures/landscape.terrain",
        RUST / "tests/terrain_fixture.rs",
        RUST / "ffi/examples/terrain_measure.rs",
    ]
    sources += [
        ROOT / "AddOn/EvP/tests/cpp" / name
        for name in ("benchmark_terrain.cpp", "terrain_fixture.hpp", "CMakeLists.txt")
    ]
    sources += [
        ROOT / "AddOn/EvP/Sources/AddOn/Geometry" / name
        for name in ("GeometryRustTerrain.cpp", "GeometryRustIdentity.cpp")
    ]
    sources += [ROOT / "tools/benchmark_terrain.py", ROOT / "tools/benchmark_composition.py"]
    source_hashes = {str(path.relative_to(ROOT)).replace("\\", "/"): sha256(path) for path in sources}
    records = []
    for fixture in FIXTURES:
        expected = None
        for process in range(args.processes):
            # Rotate first lane to limit systematic cold-first bias. Fresh process
            # per fixture/lane, one worker, three warmups. No concurrent builds.
            lanes = ("kernel", "facade") if process % 2 == 0 else ("facade", "kernel")
            for lane in lanes:
                record = run(executables[lane], lane, fixture, args.iterations)
                signature = tuple(record[k] for k in ("hash", "vertices", "triangles", "constraints"))
                expected = signature if expected is None else expected
                if signature != expected:
                    raise ValueError(f"kernel/facade/process geometry mismatch: {fixture}")
                record["process"] = process
                records.append(record)
    if any(sha256(executables[lane]) != digest for lane, digest in hashes.items()) or any(
        sha256(ROOT / path) != digest for path, digest in source_hashes.items()
    ):
        raise ValueError("binary/source changed during measurement")
    cargo_home = Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo"))
    rustup = shutil.which("rustup") or str(cargo_home / "bin" / ("rustup.exe" if os.name == "nt" else "rustup"))
    pin = tomllib.loads((RUST / "rust-toolchain.toml").read_text())["toolchain"]["channel"]
    rustc = subprocess.check_output([rustup, "run", pin, "rustc", "-vV"], text=True, timeout=30)
    report = {
        "schemaVersion": 1,
        "recordedAtUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "coreCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "dirtyStatus": subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).splitlines(),
        "sourceSha256": source_hashes,
        "binarySha256": hashes,
        "cmakeSettings": settings,
        "platform": platform.platform(),
        "processor": platform.processor(),
        "logicalCpuCount": os.cpu_count(),
        "rustcVersionVerbose": rustc.splitlines(),
        "rustProfile": "release; probes disabled during timing",
        "workerCount": 1,
        "warmupsPerProcess": 3,
        "cpuAffinityPinned": False,
        "seed": None,
        "scope": (
            "Six hand-authored neutral survey fixtures; deterministic 100x100 grid with 500 two-point "
            "existing-edge breaklines, 19602 triangles (approximately 20k), no refinement on target workload. "
            "No native captures or C++ terrain oracle."
        ),
        "timingScope": (
            "Per-call construction/validation through owned result; destruction/digest excluded. "
            "Fresh-result allocations, no buffer reuse. Single-worker wall time, no CPU isolation/power control. "
            "Upper middle median, nearest-rank p95. Facade includes catalog conversion, two Rust computations, "
            "fill/conversion/catalog retention. Kernel counting is off but allocator delegation/flag branch remains."
        ),
        "allocationScope": (
            "Separate untimed pass. Kernel System GlobalAlloc calls (including realloc) and requested bytes/peak "
            "live requested bytes, input excluded, interval balanced to zero. Facade only ordinary C++ new/new[], "
            "excludes Rust/malloc/aligned allocators. Not combined heap/allocator overhead or OS allocation counts."
        ),
        "memoryScope": (
            "Facade OS process-lifetime peak RSS includes startup, fixtures/provenance, retained expected result "
            "and warmups; not per-call peak heap. Pure kernel peak live requested bytes excludes pre-existing "
            "input and expected result."
        ),
        "copyScope": (
            "Actual copy traffic unmeasured (null). Facade source ledger reports mandatory successful-fill POD "
            "payload plus mapping conversion only; excludes catalogs, descriptors, reallocations and "
            "algorithm-internal copies. No zero-copy claim."
        ),
        "target": {
            lane: all(r["p95_ms"] < 100 for r in records if r["fixture"] == FIXTURES[-1] and r["lane"] == lane)
            for lane in executables
        },
        "fullS3aGatePassed": False,
        "remaining": (
            "Combined facade Rust+C++ allocation/peak-heap and actual copy measurement, broader/native terrain "
            "corpus and acceptance remain open. Worker classification unchanged; S3b not authorized by this report."
        ),
        "records": records,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{len(records)} fresh-process records; targets: {report['target']}; report: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
