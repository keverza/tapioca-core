"""Measurement envelope regressions; no timing thresholds or benchmark framework."""

import argparse
import json
from pathlib import Path
from types import SimpleNamespace

import benchmark_composition as bench
import pytest


def record(lane="facade", count=16, iterations=2):
    calls = iterations if lane == "facade" and count else 0
    return {
        "lane": lane,
        "count": count,
        "iterations": iterations,
        "seed": bench.SEED,
        "workerCount": 1,
        "optimized": True,
        "configuration": "RelWithDebInfo",
        "compiler": "MSVC",
        "compilerVersion": 194435217,
        "fixtureHash": str(count),
        "parityAbsoluteTolerance": 1e-12,
        "nanosecondsPerCall": 123.0,
        "checksum": 1.0,
        "cppNewCalls": calls,
        "cppNewRequestedBytes": calls * count * 96,
        "peakRssBeforeBytes": 1000,
        "peakRssAfterBytes": 2000,
    }


@pytest.mark.parametrize("lane", bench.LANES)
@pytest.mark.parametrize("count", (0, 16))
def test_valid_metadata_and_scoped_allocation_probe(lane, count):
    bench.validate(record(lane, count), lane, count, 2)


def test_large_vector_requested_padding_is_not_mislabelled_as_payload():
    item = record(count=4096)
    item["cppNewRequestedBytes"] += 39 * item["iterations"]
    bench.validate(item, "facade", 4096, 2)


@pytest.mark.parametrize(
    "key,value",
    [
        ("lane", "cpp"),
        ("count", True),
        ("iterations", 3),
        ("seed", 0),
        ("workerCount", 2),
        ("optimized", 1),
        ("configuration", "Debug"),
        ("compiler", ""),
        ("compilerVersion", "1944"),
        ("fixtureHash", "NaN"),
        ("fixtureHash", str(2**64)),
        ("parityAbsoluteTolerance", 1e-6),
        ("nanosecondsPerCall", 0),
        ("nanosecondsPerCall", float("nan")),
        ("checksum", float("inf")),
        ("cppNewCalls", 0),
        ("cppNewRequestedBytes", 96),
        ("peakRssBeforeBytes", 0),
        ("peakRssAfterBytes", 999),
    ],
)
def test_malformed_measurements_are_not_accepted_as_evidence(key, value):
    item = record()
    item[key] = value
    with pytest.raises(ValueError):
        bench.validate(item, "facade", 16, 2)


def test_fresh_process_collection_rotates_lanes_and_retains_samples(monkeypatch):
    commands = []

    def run(command, **options):
        commands.append(command)
        assert options == dict(check=True, capture_output=True, text=True, timeout=120)
        _, lane, count, iterations = command
        return SimpleNamespace(stdout=json.dumps(record(lane, int(count), int(iterations))))

    monkeypatch.setattr(bench.subprocess, "run", run)
    records = bench.collect(Path("unused-benchmark"), 3, 2)
    assert len(records) == len(commands) == 45
    assert [c[1] for c in commands[:9]] == ["cpp", "abi", "facade", "abi", "facade", "cpp", "facade", "cpp", "abi"]
    summary = bench.summarize(records)
    assert len(summary) == 15
    for row in summary:
        assert row["samples"] == 3
        assert row["nanosecondsPerCallMedian"] == 123
        assert row["cppNewCallsPerCall"] == int(row["lane"] == "facade" and row["count"] != 0)
        assert row["abiIngressCopyBytesPerCallSourceModel"] == (row["count"] * 96 if row["lane"] == "facade" else 0)


@pytest.mark.parametrize("field", ("fixtureHash", "compilerVersion", "configuration"))
def test_inconsistent_fixture_or_build_fails_before_report(monkeypatch, field):
    calls = 0

    def run(command, **options):
        nonlocal calls
        _, lane, count, iterations = command
        result = record(lane, int(count), int(iterations))
        calls += 1
        if calls == 2:
            result[field] = {"fixtureHash": "42", "compilerVersion": 1, "configuration": "Release"}[field]
        return SimpleNamespace(stdout=json.dumps(result))

    monkeypatch.setattr(bench.subprocess, "run", run)
    with pytest.raises(ValueError):
        bench.collect(Path("unused-benchmark"), 3, 2)


@pytest.mark.parametrize("text", ("0", "-1", "10001"))
def test_work_is_bounded(text):
    with pytest.raises(argparse.ArgumentTypeError):
        bench.bounded_integer(text, 10000)


@pytest.mark.parametrize("extra", ("", "CMAKE_CXX_FLAGS:STRING=/fp:fast", "EVP_SANITIZE:BOOL=ON"))
def test_build_flags_are_recorded_and_unsafe_measurement_modes_refuse(tmp_path, extra):
    cache = tmp_path / "CMakeCache.txt"
    cache.write_text(
        "TAPIOCA_LINK_RUST_GEOMETRY:BOOL=ON\nTAPIOCA_BUILD_GEOMETRY_BENCHMARKS:BOOL=ON\n"
        "EVP_SANITIZE:BOOL=OFF\nCMAKE_GENERATOR_TOOLSET:INTERNAL=v143\n"
        "CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=/O2 /Ob1 /DNDEBUG\n" + extra,
        encoding="utf-8",
    )
    if extra:
        with pytest.raises(ValueError):
            bench.build_settings(cache)
    else:
        values = bench.build_settings(cache)
        assert values["CMAKE_GENERATOR_TOOLSET"] == "v143"
        assert values["CMAKE_CXX_FLAGS_RELWITHDEBINFO"] == "/O2 /Ob1 /DNDEBUG"
