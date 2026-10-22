"""Fail-closed terrain measurement validation, without running a benchmark."""

import copy

import pytest
from benchmark_terrain import validate


def record(lane="kernel"):
    value = dict(
        lane=lane,
        fixture="grid_100x100_500",
        iterations=3,
        hash="0123456789abcdef",
        vertices=10000,
        triangles=19602,
        constraints=500,
        samples_ms=[1.0, 2.0, 3.0],
        median_ms=2.0,
        p95_ms=3.0,
        copy_bytes=None,
        rust_alloc_calls=10,
        rust_requested_bytes=100,
        rust_peak_live_requested_bytes=50,
    )
    if lane == "facade":
        value.update(
            configuration="RelWithDebInfo",
            compiler="MSVC",
            compiler_version=195000000,
            cpp_new_calls=10,
            cpp_new_requested_bytes=100,
            known_fill_payload_copy_bytes=50,
            known_mapping_conversion_bytes=10,
            process_peak_rss_before_bytes=100,
            process_peak_rss_after_bytes=200,
            rust_alloc_calls=None,
            combined_peak_heap_bytes=None,
        )
    return value


@pytest.mark.parametrize("lane", ["kernel", "facade"])
def test_valid(lane):
    validate(record(lane), lane, "grid_100x100_500", 3)


@pytest.mark.parametrize(
    "key,value",
    [
        ("vertices", 9999),
        ("constraints", 499),
        ("triangles", 20000),
        ("hash", "bad"),
        ("iterations", True),
        ("samples_ms", [1, float("nan"), 3]),
        ("samples_ms", [1, -2, 3]),
        ("median_ms", 9),
        ("p95_ms", float("inf")),
        ("copy_bytes", 0),
        ("rust_alloc_calls", 0),
        ("rust_peak_live_requested_bytes", 101),
    ],
)
def test_invalid_kernel(key, value):
    data = copy.deepcopy(record())
    data[key] = value
    with pytest.raises(ValueError):
        validate(data, "kernel", "grid_100x100_500", 3)


@pytest.mark.parametrize(
    "key,value",
    [
        ("configuration", "Debug"),
        ("cpp_new_calls", 0),
        ("rust_alloc_calls", 1),
        ("combined_peak_heap_bytes", 0),
        ("process_peak_rss_after_bytes", 50),
        ("compiler", ""),
    ],
)
def test_invalid_facade(key, value):
    data = record("facade")
    data[key] = value
    with pytest.raises(ValueError):
        validate(data, "facade", "grid_100x100_500", 3)
