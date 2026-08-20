"""evp.ui.progress / evp.ui.stages — the run-progress channel (PLAT-F8's Python half).

Kept OUTSIDE PyPackage so it never ships with the add-on. Run:
    python -m pytest AddOn/EvP/tests/test_ui_progress.py

Each of these guards something that fails QUIETLY in Archicad, where the only
symptom is a status line that says the wrong thing for minutes:

  * the throttle: a per-item loop calling progress() must not turn into one bus
    call per item — but a STAGE line must never be the one that gets dropped, or
    the palette skips from stage 1 to stage 3;
  * the position: "[3/8]" is the only part of the display the user can trust to
    mean how much is left, so a stage list that is declared and then not advanced
    (or advanced past its end) must not produce a nonsense count;
  * the exit path: an exception owns the status line on the way out — the
    palette's own FinishRun writes the failure a moment later, and a "done"
    written in between just flickers over it.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))

from evp import ui   # noqa: E402


@pytest.fixture
def sent(monkeypatch):
    """Capture what reaches the bus, and start with the throttle open."""
    lines = []
    monkeypatch.setattr(ui, "status", lines.append)
    monkeypatch.setattr(ui, "_last_emit", 0.0)
    return lines


def test_progress_formats_the_position(sent):
    ui.progress("Rendering", 3, 8, force=True)
    assert sent == ["[3/8] Rendering"]


def test_progress_without_a_total_still_counts(sent):
    ui.progress("Roof", 4, force=True)
    assert sent == ["[4] Roof"]


def test_hint_is_appended(sent):
    ui.progress("Sampling", force=True, hint="do not pan or zoom")
    assert sent == ["Sampling — do not pan or zoom"]


def test_a_tight_loop_is_throttled(sent):
    for i in range(500):
        ui.progress("item", i, 500)
    # One got through; the rest were inside the 0.25 s window. The exact number
    # depends on how fast the loop runs, which is the point — it is bounded by
    # TIME, not by iteration count.
    assert 1 <= len(sent) <= 3


def test_force_is_never_throttled(sent):
    for i in range(10):
        ui.progress("stage", i, 10, force=True)
    assert len(sent) == 10


def test_stages_number_every_line(sent):
    st = ui.stages(["Read", "Fit", "Place"], title="Slope symbols")
    st.next()
    st.next()
    assert sent == ["[1/3] Slope symbols: Read", "[2/3] Slope symbols: Fit"]


def test_stage_label_may_be_decided_at_run_time(sent):
    st = ui.stages(["Storeys"], title="Sweep")
    assert st.next(label="Storey 3 of 8") == "Storey 3 of 8"
    assert sent == ["[1/1] Sweep: Storey 3 of 8"]


def test_next_past_the_end_does_not_overrun_the_count(sent):
    st = ui.stages(["one"])
    st.next()
    st.next()
    # The position saturates rather than reporting "[2/1]" — a count that exceeds
    # its own total is worse than a stalled one, because it reads as a bug in the
    # command rather than in the stage list.
    assert sent == ["[1/1] one", "[1/1] one"]


def test_note_keeps_the_stage_position(sent):
    st = ui.stages(["Measure", "Report"])
    st.next()
    monkey = ui._last_emit
    ui._last_emit = 0.0          # notes are throttled; open the window for the test
    st.note("roof 7 of 40")
    assert ui._last_emit != monkey
    assert sent[-1] == "[1/2] roof 7 of 40"


def test_echo_receives_the_stage_lines(sent):
    echoed = []
    st = ui.stages(["A", "B"], echo=echoed.append)
    st.next()
    st.next()
    assert echoed == ["[1/2] A", "[2/2] B"]


def test_echo_is_not_spammed_by_notes(sent):
    echoed = []
    st = ui.stages(["A"], echo=echoed.append)
    st.next()
    ui._last_emit = 0.0
    st.note("detail")
    # The log file is the durable record of the run's SHAPE. A throttled note is
    # a transient, and one per item would bury the stage boundaries it sits under.
    assert echoed == ["[1/1] A"]


def test_context_manager_summary_on_success(sent):
    with ui.stages(["A"]) as st:
        st.next()
        st.finish("42 roofs done")
    assert sent[-1] == "42 roofs done"


def test_an_exception_leaves_the_status_line_alone(sent):
    with pytest.raises(ValueError):
        with ui.stages(["A"]) as st:
            st.next()
            raise ValueError("boom")
    assert sent == ["[1/1] A"]
