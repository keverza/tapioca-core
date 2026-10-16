import json
import math

import pytest

from tools.archicad_client import normalized_guid


def assert_metrics(result, fixture):
    expected = fixture["expected"]
    assert result["basis"] == "unclipped_slab_prisms"
    rows = result["rows"]
    assert rows and len(rows) == result["floorCount"] == expected["floorCount"]
    assert {normalized_guid(row["guid"]) for row in rows} == {normalized_guid(guid) for guid in fixture["elementGuids"]}
    tolerance = fixture.get("absoluteTolerance", 1e-6)
    assert math.isfinite(tolerance) and 0 < tolerance <= 0.01
    for key in ("rawAreaM2", "rawVolumeM3"):
        assert math.isfinite(expected[key]) and expected[key] > 0
        assert math.isfinite(result[key])
        assert result[key] == pytest.approx(expected[key], abs=tolerance, rel=0)
    for row in rows:
        assert all(math.isfinite(row[key]) for key in ("areaM2", "volumeM3", "z", "height"))
        assert row["areaM2"] > 0 and row["height"] > 0 and row["volumeM3"] > 0
    assert sum(row["areaM2"] for row in rows) == pytest.approx(result["rawAreaM2"], abs=tolerance, rel=0)
    assert sum(row["volumeM3"] for row in rows) == pytest.approx(result["rawVolumeM3"], abs=tolerance, rel=0)


def test_massing_numeric_results_and_repeatability(live_client, live_runner):
    options = live_runner.options
    fixture = json.loads(options.fixture.read_text(encoding="utf-8"))
    guids = fixture["elementGuids"]
    assert 1 <= len(guids) <= 8
    assert len({normalized_guid(guid) for guid in guids}) == len(guids)
    assert fixture["expected"]["floorCount"] > 0
    assert all(live_client.health["tapiocaCapabilities"].values()), "Bridge unavailable"
    request = {"workflow": "massing_feasibility", "inputs": {"elementGuids": guids}, "mode": "preview", "seed": 42}
    results = []
    try:
        for _ in range(2):
            live_client.verify_project(options.expected_project)
            job = live_client.addon("TapiocaTest", "RunWorkflow", request)
            result = live_client.wait_for_job(job["jobId"], options.workflow_timeout)
            live_client.verify_project(options.expected_project)
            assert_metrics(result, fixture)
            results.append(result)
        assert results[0] == results[1], "Identical explicit inputs produced different numeric results"
    finally:
        # Test state only: never cancels production UI work or changes project data.
        try:
            live_client.addon("TapiocaTest", "Reset")
        except Exception as exc:
            live_runner.errors.append(f"Test reset failed: {exc}")
