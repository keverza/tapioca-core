"""Protect compact/manual display clients at the SDK-free wire boundary."""

import importlib.util
import json
from pathlib import Path

import pytest


@pytest.fixture
def wire(monkeypatch):
    path = Path(__file__).with_name("dryrun_command.py")
    spec = importlib.util.spec_from_file_location("sunstudy_speedup_wire", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.setattr(module, "_SCENARIO", "sunstudy_display_async")
    return module


def _completed_day(wire):
    started = wire._one("EvP.StartSunStudy", {"backend": "gpu", "timestep": 15})["data"]
    assert started["backend"] == "gpu"
    wire._one("EvP.AdvanceSunStudy", {"maxSteps": 100})
    return started


def test_compact_summary_matches_the_full_day_without_bulk_fields(wire):
    started = _completed_day(wire)
    detailed = wire._one("EvP.GetSunStudyResults", {"includePositions": True, "includeAtlas": True})["data"]
    summary = wire._one("EvP.GetSunStudyResults", {"summaryOnly": True})["data"]
    hours = detailed["hours"]
    assert summary["summaryOnly"] and summary["converged"]
    assert summary["count"] == len(hours)
    assert summary["hours"] == []
    assert summary["minHours"] == min(hours)
    assert summary["meanHours"] == sum(hours) / len(hours)
    assert summary["maxHours"] == max(hours)
    assert summary["daylightHours"] == started["daylightHours"]
    assert summary["fullyLit"] == sum(h >= started["daylightHours"] - 1e-9 for h in hours)
    assert summary["fullyShaded"] == sum(h <= 1e-9 for h in hours)
    for key in ("positions", "normals", "atlasPacked", "stepBits", "positionsPacked"):
        assert key not in summary


@pytest.mark.parametrize("bulk", ["includePositions", "includeAtlas", "includeSteps"])
def test_summary_refuses_conflicting_bulk_options(wire, bulk):
    _completed_day(wire)
    result = wire._one("EvP.GetSunStudyResults", {"summaryOnly": True, bulk: True})
    assert not result["ok"]


def test_async_display_retains_previous_overlay_until_the_ready_packet_is_consumed(wire):
    started = _completed_day(wire)
    wire._SUN_OVERLAY.update(studyId="old-overlay", viewerRunning=True, drawing=True)
    shown = wire._one("EvP.ShowSunStudy", {"studyId": started["studyId"]})["data"]
    assert shown["preparing"]
    pending = wire._one("EvP.SunStudyOverlayState", {})["data"]
    assert pending["studyId"] == "old-overlay"
    assert pending["preparing"] and pending["pendingStudyId"] == started["studyId"]
    ready = wire._one("EvP.SunStudyOverlayState", {})["data"]
    assert not ready["preparing"] and ready["drawing"]
    assert ready["studyId"] == started["studyId"]


def test_hide_discards_a_pending_display_instead_of_resurrecting_it(wire):
    started = _completed_day(wire)
    wire._one("EvP.ShowSunStudy", {"studyId": started["studyId"]})
    wire._one("EvP.ShowSunStudy", {"show": False})
    for _ in range(3):
        state = wire._one("EvP.SunStudyOverlayState", {})["data"]
        assert not state["preparing"] and not state["drawing"]
        assert state["studyId"] == ""


def test_record_cancellation_discards_preparation_but_keeps_the_previous_image(wire):
    started = _completed_day(wire)
    wire._SUN_OVERLAY.update(studyId="old-overlay", viewerRunning=True, drawing=True)
    wire._one("EvP.ShowSunStudy", {"studyId": started["studyId"]})
    wire._one("EvP.CancelSunStudy", {"studyId": started["studyId"]})
    state = wire._one("EvP.SunStudyOverlayState", {})["data"]
    assert not state["preparing"] and state["drawing"]
    assert state["studyId"] == "old-overlay"


@pytest.mark.parametrize("detailed", [False, True])
def test_manual_smoke_selects_backend_and_waits_for_worker_display(wire, monkeypatch, detailed):
    folder = Path(__file__).parents[4] / "private" / "Diagnostics" / "Commands" / "SunStudyNativeSmoke"
    if not (folder / "command.py").is_file():
        pytest.skip("the private diagnostic command is not present in this clone")
    smoke = wire.load(str(folder))
    calls = []

    def transport(command, params_json):
        calls.append((command, json.loads(params_json)))
        return wire.transport(command, params_json)

    monkeypatch.setattr(wire.evp_api, "_transport", lambda: transport)
    monkeypatch.setattr(smoke, "_flush", lambda: None)
    smoke.run(backend="gpu", domain="patch", display="roles", timestep="15", detailed_results=detailed)
    text = "\n".join(smoke._lines)
    assert "NO PASS" not in text
    assert "PASS: the renderer is drawing this study" in text
    assert not wire.unknown
    assert next(p for c, p in calls if c == "Tapioca.BuildSnapshot")["reuseShared"]
    assert next(p for c, p in calls if c == "Tapioca.StartSunStudy")["backend"] == "gpu"
    results = next(p for c, p in calls if c == "Tapioca.GetSunStudyResults")
    assert bool(results.get("summaryOnly")) != detailed
    assert bool(results.get("includePositions")) == detailed
    assert bool(results.get("includeAtlas")) == detailed
    assert sum(c == "Tapioca.SunStudyOverlayState" for c, _ in calls) >= 2
