"""Keep the fake display wire honest about complete-day coarse previews."""

import importlib.util
from pathlib import Path

import pytest


@pytest.fixture
def wire():
    path = Path(__file__).with_name("dryrun_command.py")
    spec = importlib.util.spec_from_file_location("sunstudy_preview_wire", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module._SUN_STUDY.update(id="coarse-preview", resolved=1, total=49)
    return module


def test_partial_day_cannot_be_labelled_as_a_complete_day_preview(wire):
    wire._SUN_OVERLAY.update(studyId="previous-cache")
    result = wire._one("EvP.ShowSunStudy", {"studyId": "coarse-preview", "preview": True})
    assert not result["ok"]
    assert "complete day" in result["error"]["message"]
    assert wire._SUN_OVERLAY["studyId"] == "previous-cache"


def test_complete_day_preview_is_explicit_in_both_display_and_renderer_state(wire):
    wire._SUN_STUDY["resolved"] = 49
    result = wire._one("EvP.ShowSunStudy", {"studyId": "coarse-preview", "preview": True})
    assert result["ok"]
    assert result["data"]["converged"]
    assert result["data"]["preview"]
    state = wire._one("EvP.SunStudyOverlayState", {})
    assert state["data"]["preview"]
    final = wire._one("EvP.ShowSunStudy", {"studyId": "coarse-preview"})
    assert not final["data"]["preview"]
    assert not wire._one("EvP.SunStudyOverlayState", {})["data"]["preview"]


def test_clearing_a_preview_clears_its_label_and_wake_diagnostics_remain_available(wire):
    wire._SUN_STUDY["resolved"] = 49
    wire._one("EvP.ShowSunStudy", {"studyId": "coarse-preview", "preview": True})
    cleared = wire._one("EvP.ShowSunStudy", {"show": False})
    assert not cleared["data"]["preview"]
    assert not wire._one("EvP.SunStudyOverlayState", {})["data"]["preview"]
    follower = wire._one("EvP.SunStudyFollowerState", {})["data"]
    assert follower["completionWakes"] == 0
    assert follower["completionWakeFailures"] == 0
