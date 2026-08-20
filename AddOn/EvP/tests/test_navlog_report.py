"""The desync analysis must recover a lag that was put in on purpose.

WHY THIS TEST EXISTS.  ``navlog_report.py`` turns two logged camera streams into
one number -- "the overlay is N pixels out, and M milliseconds behind".  That
number is about to be used to accept or reject every remaining option on the
camera-sync ladder, so it has to be falsifiable.  A synthetic log with a KNOWN
lag baked in is the only way to say the analysis is measuring what it claims:
against real data every answer looks plausible, because there is nothing to
check it against.

⚠️ IT TESTS THE MEASURING DEVICE, NOT THE OVERLAY.  Nothing here says the
overlay is fast.  It says that IF the overlay is 45 ms behind, this tool will
report 45 ms -- and, just as importantly, that if the overlay is AHEAD (which is
what a working ``predict`` mode looks like) the tool reports a negative lag
rather than clamping it to zero and calling the difference jitter.
"""

import importlib.util
import math
import sys
import os

import pytest

_TOOL = os.path.join(os.path.dirname(__file__), "..", "tools", "navlog_report.py")


def _load_tool():
    spec = importlib.util.spec_from_file_location("navlog_report", os.path.abspath(_TOOL))
    module = importlib.util.module_from_spec(spec)
    # ⚠️ REGISTERED BEFORE exec_module, NOT AFTER. `@dataclass` resolves its
    # annotations through `sys.modules[cls.__module__]`, so a module executed
    # while absent from that table dies inside dataclasses with
    # "'NoneType' object has no attribute '__dict__'" -- which reads as a bug in
    # the tool rather than in how the test imported it.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# The fixture's geometry. A 1000 px surface showing 20 m means 0.02 m per pixel,
# so a pan at 500 px/s is 10 m/s -- brisk but ordinary, and it makes the expected
# pixel error a round number: 45 ms of lag is 22.5 px.
_HEIGHT_PX = 1000
_HALF_HEIGHT_M = 10.0
_METRES_PER_PIXEL = _HALF_HEIGHT_M * 2.0 / _HEIGHT_PX
_PAN_PIXELS_PER_SECOND = 500.0
_PAN_METRES_PER_SECOND = _PAN_PIXELS_PER_SECOND * _METRES_PER_PIXEL


def _write_fixture(path, cells):
    """A log in exactly the shape ``navlog::Start`` writes.

    `cells` is a list of (label, lag_ms). Each gets its own marked slice with a
    steady pan and the overlay displaced in TIME by `lag_ms` -- positive meaning
    it shows where Archicad was earlier.
    """
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n# ---- ArchViz navigation comparison, new session ----\n")
        handle.write("t_ms,gap_ms,source,window,mode,eyeX,eyeY,eyeZ,tgtX,tgtY,tgtZ,"
                     "dist,azimuth_deg,roll_or_pitch_deg,cone_or_fov_deg,extra\n")
        base = 0
        for label, lag_ms in cells:
            handle.write(f"{base},0,mark,-,-,0,0,0,0,0,0,0,0,0,0,{label}/start\n")
            for step in range(200):
                t = base + step * 15
                centre = _PAN_METRES_PER_SECOND * (t / 1000.0)
                handle.write(f"{t},16,archicad,FloorPlan,plan,0,0,0,{centre:.6f},0,0,"
                             f"{_HALF_HEIGHT_M:.6f},0,0,0,\n")
                # The overlay's frame lands a little after the sample it used --
                # as it does live, since the render thread presents after the
                # poll -- and shows where Archicad was `lag_ms` ago.
                shown = _PAN_METRES_PER_SECOND * ((t + 7 - lag_ms) / 1000.0)
                handle.write(f"{t + 7},16,viewer,FloorPlan,plan,0,0,0,{shown:.6f},0,0,"
                             f"{_HALF_HEIGHT_M:.6f},0,0,0,h={_HEIGHT_PX};w=1600;frame={step}\n")
            handle.write(f"{base},0,mark,-,-,0,0,0,0,0,0,0,0,0,0,{label}/end\n")
            base += 5000


def _cells_by_label(module, path):
    cells, _notes = module.parse(path)
    return {cell.label: cell for cell in cells if cell.viewer_rows}


@pytest.mark.parametrize("label,lag_ms", [
    ("behind45", 45.0),
    ("perfect", 0.0),
    # ⚠️ NEGATIVE ON PURPOSE. A predictor that works is AHEAD of Archicad. A
    # search that only looked forward in time would report its lag as zero and
    # its whole error as residual -- reading as "jittery" when it is early, which
    # is the exact distinction `predict` vs `legacy` is meant to settle.
    ("ahead20", -20.0),
])
def test_recovers_the_lag_that_was_put_in(tmp_path, label, lag_ms):
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_fixture(path, [(label, lag_ms)])

    cell = _cells_by_label(module, path)[label]
    recovered, residual = module.best_fit_latency(cell.viewer_rows, cell.archicad_samples)

    # The search steps in 2 ms, so it can never be closer than that.
    assert recovered == pytest.approx(lag_ms, abs=3.0)
    # With a clean synthetic ramp, nothing should be left once the shift is
    # applied. A large residual here would mean the shift is fitting noise.
    assert residual < 1.0


def test_reports_the_desync_in_pixels_of_the_view(tmp_path):
    # ⚠️ PIXELS ARE THE UNIT THE VERDICT IS IN, so the conversion is worth
    # pinning: at 500 px/s a 45 ms lag is 22.5 px, and if the metres-per-pixel
    # conversion ever inverts, this is what says so rather than a plausible
    # wrong number.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_fixture(path, [("behind45", 45.0)])

    cell = _cells_by_label(module, path)["behind45"]
    rows = module._desync_pixels(cell.viewer_rows, cell.archicad_samples, 0.0)
    assert rows
    totals = sorted(row[3] for row in rows)
    expected = _PAN_PIXELS_PER_SECOND * 0.045
    assert module.percentile(totals, 0.50) == pytest.approx(expected, rel=0.05)


def test_a_perfectly_synced_overlay_measures_as_zero(tmp_path):
    # The null case. If this ever reports a non-zero lag, every other number the
    # tool produces carries that same bias and none of them mean anything.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_fixture(path, [("perfect", 0.0)])

    cell = _cells_by_label(module, path)["perfect"]
    rows = module._desync_pixels(cell.viewer_rows, cell.archicad_samples, 0.0)
    totals = [row[3] for row in rows]
    assert max(totals) < 0.5


def test_interpolation_refuses_to_extrapolate_past_the_samples(tmp_path):
    # ⚠️ EXTRAPOLATION WOULD INVENT THE MOTION BEING MEASURED. Beyond the ends of
    # the ground-truth stream there is no truth, and a straight-line guess there
    # would be indistinguishable from a real reading.
    module = _load_tool()
    # 16 ms apart: one ordinary poll interval, well inside the gap limit.
    samples = [(0.0, 0.0, 0.0, 10.0, 0.0), (16.0, 1.0, 0.0, 10.0, 0.0)]
    assert module._interpolate(samples, -1.0) is None
    assert module._interpolate(samples, 17.0) is None
    midpoint = module._interpolate(samples, 8.0)
    assert midpoint is not None
    assert midpoint[0] == pytest.approx(0.5)


def test_interpolation_refuses_to_cross_a_long_gap(tmp_path):
    # ⚠️ A STRAIGHT LINE ACROSS A LONG GAP IS A FICTION, and it is worst exactly
    # where the interesting cells are: an abrupt stop or a reversal INSIDE the
    # gap is smoothed into steady motion, so the overlay gets compared against a
    # camera Archicad never had -- and the error shows up as the overlay's fault.
    module = _load_tool()
    samples = [(0.0, 0.0, 0.0, 10.0, 0.0), (500.0, 100.0, 0.0, 10.0, 0.0)]
    assert module._interpolate(samples, 250.0) is None


def test_the_lag_fit_scores_every_shift_over_the_same_frames(tmp_path):
    # ⚠️ OTHERWISE A SHIFT CAN WIN BY LOSING THE HARD FRAMES. Measurability
    # depends on the shifted timestamp landing inside the ground-truth stream, so
    # each shift used to drop a different set at the ends -- biasing the fit
    # toward large shifts, which is the exact direction the predict modes are
    # judged in.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_fixture(path, [("behind45", 45.0)])
    cell = _cells_by_label(module, path)["behind45"]

    lag, residual = module.best_fit_latency(cell.viewer_rows, cell.archicad_samples)
    assert lag == pytest.approx(45.0, abs=3.0)
    assert residual < 1.0


def test_a_log_with_no_presented_frames_says_so_rather_than_reporting_zero(tmp_path, capsys):
    # A run with the nav log on but no overlay up produces archicad rows and no
    # viewer rows. Reporting "0 px desync" for that would be the most dangerous
    # possible output: a perfect score for a measurement that never happened.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n# ---- ArchViz navigation comparison, new session ----\n")
        handle.write("t_ms,gap_ms,source,window,mode,eyeX,eyeY,eyeZ,tgtX,tgtY,tgtZ,"
                     "dist,azimuth_deg,roll_or_pitch_deg,cone_or_fov_deg,extra\n")
        for step in range(10):
            handle.write(f"{step * 15},16,archicad,FloorPlan,plan,0,0,0,0,0,0,10,0,0,0,\n")

    cells, _notes = module.parse(path)
    module.desync_report(cells, as_csv=False)
    printed = capsys.readouterr().out
    assert "NO DESYNC MEASUREMENT" in printed


def _write_still_fixture(path, label):
    """A cell where the camera never moves -- the idle cells of a real matrix run."""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n# ---- ArchViz navigation comparison, new session ----\n")
        handle.write("t_ms,gap_ms,source,window,mode,eyeX,eyeY,eyeZ,tgtX,tgtY,tgtZ,"
                     "dist,azimuth_deg,roll_or_pitch_deg,cone_or_fov_deg,extra\n")
        handle.write(f"0,0,mark,-,-,0,0,0,0,0,0,0,0,0,0,{label}/start\n")
        for step in range(200):
            t = step * 15
            handle.write(f"{t},16,archicad,FloorPlan,plan,0,0,0,0,0,0,"
                         f"{_HALF_HEIGHT_M:.6f},0,0,0,\n")
            handle.write(f"{t + 7},16,viewer,FloorPlan,plan,0,0,0,0,0,0,"
                         f"{_HALF_HEIGHT_M:.6f},0,0,0,h={_HEIGHT_PX};w=1600;frame={step}\n")
        handle.write(f"0,0,mark,-,-,0,0,0,0,0,0,0,0,0,0,{label}/end\n")


def test_a_motionless_cell_refuses_to_report_a_lag(tmp_path):
    # ⚠️ THE REAL RUN'S IDLE CELLS ALL REPORTED -200 ms, which is simply the first
    # shift the search tries. With no motion every shift scores zero error, so the
    # search returns an arbitrary answer -- and it looked exactly like the honest
    # numbers in the rows above it. A confident wrong number in a decision table is
    # worse than a blank.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_still_fixture(path, "idle")

    cell = _cells_by_label(module, path)["idle"]
    lag, _residual = module.best_fit_latency(cell.viewer_rows, cell.archicad_samples)
    assert module.fit_quality(cell.viewer_rows, cell.archicad_samples, lag) == "still"


def test_a_lag_past_the_search_window_is_flagged_not_reported(tmp_path):
    # A fit landing on the last shift tried means the true lag is somewhere BEYOND
    # the window. Reporting it as exactly 200 ms would state a precision the search
    # cannot support.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_fixture(path, [("waybehind", 400.0)])

    cell = _cells_by_label(module, path)["waybehind"]
    lag, _residual = module.best_fit_latency(cell.viewer_rows, cell.archicad_samples)
    assert module.fit_quality(cell.viewer_rows, cell.archicad_samples, lag) == "saturated"


def test_a_real_lag_still_reads_as_a_usable_fit(tmp_path):
    # The guard must not swallow the measurements it exists to protect.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_fixture(path, [("behind45", 45.0)])

    cell = _cells_by_label(module, path)["behind45"]
    lag, _residual = module.best_fit_latency(cell.viewer_rows, cell.archicad_samples)
    assert module.fit_quality(cell.viewer_rows, cell.archicad_samples, lag) == "ok"


def test_poll_rate_is_measured_from_the_samples(tmp_path):
    # The fixture polls every 15 ms, so the ceiling it reports must be ~66 Hz. This
    # column is how a starved poll is told apart from a slow renderer, so a wrong
    # conversion here would misattribute the whole diagnosis.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    _write_fixture(path, [("behind45", 45.0)])

    cell = _cells_by_label(module, path)["behind45"]
    assert module.truth_hz(cell.archicad_samples) == pytest.approx(1000.0 / 15.0, rel=0.02)


# ---------------------------------------------------------------------------
# Adversarial fixtures (review item 16).
#
# ⚠️ THE EXISTING FIXTURES ARE A CLEAN CONSTANT-VELOCITY PAN, and a tool that
# only ever sees one is validated against the easiest case it will meet. Every
# case below is one the real runs actually contain and where a plausible-looking
# wrong answer is most likely.

def _write_rows(path, label, rows):
    """rows: list of (t_ms, source, mode, fields dict, extra)."""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n# ---- ArchViz navigation comparison, new session ----\n")
        handle.write("# throttle_ms=0\n")
        handle.write("t_ms,gap_ms,source,window,mode,eyeX,eyeY,eyeZ,tgtX,tgtY,tgtZ,"
                     "dist,azimuth_deg,roll_or_pitch_deg,cone_or_fov_deg,extra\n")
        handle.write(f"0,0,mark,-,-,0,0,0,0,0,0,0,0,0,0,{label}/start\n")
        for t, source, mode, f, extra in rows:
            handle.write("%d,16,%s,W,%s,%s,%s,%s,%s,%s,%s,%s,%s,0,%s,%s\n" % (
                t, source, mode,
                f.get("eyeX", 0), f.get("eyeY", 0), f.get("eyeZ", 0),
                f.get("tgtX", 0), f.get("tgtY", 0), f.get("tgtZ", 0),
                f.get("dist", 10), f.get("azimuth_deg", 0),
                f.get("cone", 0), extra))
        handle.write(f"0,0,mark,-,-,0,0,0,0,0,0,0,0,0,0,{label}/end\n")


def test_a_gap_in_the_truth_stream_is_not_bridged(tmp_path):
    # An abrupt stop INSIDE a poll gap: truth jumps, then holds. Interpolating
    # across it would invent a smooth ramp and blame the overlay for the
    # difference.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    rows = []
    for step in range(10):
        rows.append((step * 16, "archicad", "plan", {"tgtX": step * 0.1}, ""))
        rows.append((step * 16 + 7, "viewer", "plan", {"tgtX": step * 0.1},
                     "h=1000;w=1600;frame=%d" % step))
    # 400 ms of nothing, then resume far away.
    rows.append((600, "archicad", "plan", {"tgtX": 50.0}, ""))
    rows.append((500, "viewer", "plan", {"tgtX": 1.0}, "h=1000;w=1600;frame=99"))
    _write_rows(path, "gap", rows)

    cell = _cells_by_label(module, path)["gap"]
    measured = module._desync_pixels(cell.viewer_rows, cell.archicad_samples, 0.0)
    # Two frames fall in the hole: the one at t=500 obviously, and the LAST of
    # the ten (t=151), which sits just past the final pre-gap sample at t=144.
    # Both must be dropped rather than scored against a fabricated camera.
    assert len(measured) == 9


def test_a_reversal_is_not_smoothed_into_continuing_motion(tmp_path):
    # The cell prediction fails in. Truth goes out and comes straight back; a
    # tool that mishandles it reports the overlay as wrong when it was right.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    rows = []
    for step in range(20):
        x = (step * 0.1) if step < 10 else ((19 - step) * 0.1)
        rows.append((step * 16, "archicad", "plan", {"tgtX": x}, ""))
        rows.append((step * 16 + 1, "viewer", "plan", {"tgtX": x},
                     "h=1000;w=1600;frame=%d" % step))
    _write_rows(path, "reversal", rows)

    cell = _cells_by_label(module, path)["reversal"]
    rows_out = module._desync_pixels(cell.viewer_rows, cell.archicad_samples, 0.0)
    # The overlay is showing exactly the truth, so a correct tool reports ~0
    # however sharp the reversal is.
    assert max(r[3] for r in rows_out) < 1.0


def test_rotation_across_pi_does_not_report_a_full_turn(tmp_path):
    # ⚠️ AN ANGLE THAT WRAPS IS THE CLASSIC FABRICATED ERROR: 179 deg -> -179 is
    # two degrees of motion, and a tool that subtracts naively calls it 358.
    module = _load_tool()
    path = str(tmp_path / "nav.log")
    rows = [
        (0,  "archicad", "plan", {"azimuth_deg": 179.0}, ""),
        (16, "archicad", "plan", {"azimuth_deg": -179.0}, ""),
        (8,  "viewer",   "plan", {"azimuth_deg": 180.0}, "h=1000;w=1600;frame=0"),
    ]
    _write_rows(path, "wrap", rows)

    cell = _cells_by_label(module, path)["wrap"]
    out = module._desync_pixels(cell.viewer_rows, cell.archicad_samples, 0.0)
    assert out
    # One degree of rotation over a 1000 px view is well under 20 px; an
    # unwrapped subtraction would produce thousands.
    assert max(r[3] for r in out) < 20.0


def test_the_grid_metric_agrees_with_a_hand_computed_displacement(tmp_path):
    # A pure 1 m pan at 0.02 m/px must be exactly 50 px everywhere on the grid --
    # no maxima-summing, no approximation.
    module = _load_tool()
    row = (0.0, 0.0, 0.0, 10.0, 0.0, 1000)
    assert module._plan_grid_pixels(row, (1.0, 0.0, 10.0, 0.0)) == pytest.approx(50.0)
    assert module._plan_grid_pixels(row, (0.0, 0.0, 10.0, 0.0)) == pytest.approx(0.0)


def test_the_grid_metric_is_never_larger_than_the_parameter_sum(tmp_path):
    # ⚠️ THE RELATIONSHIP THAT MAKES BOTH COLUMNS READABLE TOGETHER. The old sum
    # adds three maxima that need not coincide, so it is an upper bound on any
    # real point's displacement. If the grid ever exceeded it, one of the two is
    # wrong.
    module = _load_tool()
    row = (0.0, 0.3, -0.2, 10.5, 0.02, 1000)
    truth = (0.0, 0.0, 10.0, 0.0)
    grid = module._plan_grid_pixels(row, truth)
    rows = module._desync_pixels([row], [(0.0,) + truth, (16.0,) + truth], 0.0)
    assert rows
    assert grid <= rows[0][3] + 1e-6


def test_perspective_scoring_is_zero_for_identical_cameras():
    module = _load_tool()
    eye, target = (0.0, -10.0, 0.0), (0.0, 0.0, 0.0)
    row = (0.0, eye, target, 60.0, 1600, 1000)
    assert module._persp_grid_pixels(row, (eye, target, 60.0)) == pytest.approx(0.0)


def test_perspective_scoring_refuses_a_degenerate_camera():
    # eye == target has no view direction. Returning a number here would be
    # inventing one.
    module = _load_tool()
    row = (0.0, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 60.0, 1600, 1000)
    assert module._persp_grid_pixels(row, ((0.0, -1.0, 0.0), (0.0, 0.0, 0.0), 60.0)) is None
