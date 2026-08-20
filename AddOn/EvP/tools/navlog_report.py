"""Slice ``logs\\archviz_nav.log`` into matrix cells and report the poll gaps.

WHAT QUESTION THIS ANSWERS.  The camera-sync ladder (PLAT-RE54 and successors)
is judged by eye -- a human says whether the overlay trails.  That verdict is
the gate, but on its own it cannot say *why* a cell failed, and two rungs that
both "trail a bit" are indistinguishable.  This is the free diagnostic that
rides along: for each cell, how often did Archicad's camera actually get
sampled, and how bad was the worst gap.

⚠️ THE GAP IS THE MEASUREMENT, NOT AN ERROR COUNT.  ``WM_TIMER`` is a
low-priority message -- Windows generates it only when the queue is otherwise
empty -- so during a fast drag inside Archicad the poll can be starved for a
long time.  A large p95 here means the sample stream is starved, which sends the
ladder to PLAT-RE75 (the wake signal) before anything can be predicted from it.
A tight p95 with a bad eye verdict means the opposite: the samples are fine and
the problem is structural, so prediction or compositing is the answer.

⚠️ DRAGGING AND IDLE ARE REPORTED SEPARATELY, AND THAT IS THE WHOLE POINT.  The
2026-08-06 run was dismissed on its total sample count ("2603 samples, so the
poll is not starved"), which does not follow: a poll that runs at 60 Hz while
nothing is happening and stalls for 200 ms during every drag produces a large,
healthy-looking total.  Any single-number summary of this file is misleading.

The schema is written by ``navlog::Start`` in ``ArchViz/NavLog.cpp``.  Read it
there rather than trusting this docstring if the two ever disagree.

Usage::

    python AddOn/EvP/tools/navlog_report.py                    # the live log
    python AddOn/EvP/tools/navlog_report.py <path-to-log>
    python AddOn/EvP/tools/navlog_report.py --csv              # machine-readable
"""

from __future__ import annotations

import argparse
import itertools
import math
import os
import sys
from dataclasses import dataclass, field

# The columns written by navlog::Start, in order.
COLUMNS = (
    "t_ms", "gap_ms", "source", "window", "mode",
    "eyeX", "eyeY", "eyeZ", "tgtX", "tgtY", "tgtZ",
    "dist", "azimuth_deg", "roll_or_pitch_deg", "cone_or_fov_deg", "extra",
)

# A cell is "dragging" if the camera actually moved between consecutive samples.
# ⚠️ INFERRED, NOT RECORDED.  Archicad does not tell the add-on that a drag is in
# progress, and the probe cannot know either -- it only knows which gesture it
# ASKED for.  Movement is the honest proxy: it is what distinguishes "the user is
# navigating" from "the user is reading the next instruction", which is the only
# distinction this split has to support.  A cell whose gesture is a pan but whose
# rows never move is a finding in itself, not a broken heuristic.
#
# ⚠️ NOT 1e-6, WHICH IS WHAT IT WAS AND WHAT THE FIRST RUN CAUGHT: the idle cell,
# performed with hands off the mouse, reported 38 MOVING samples. The plan camera
# is a least-squares fit over three `PointToCoord` results resampled every tick,
# so its parameters wobble in the last few digits even when nothing has moved.
# 1 mm of model space is below anything visible at any plan zoom and far above
# that wobble.
MOVED_EPSILON = 1e-3


@dataclass
class Cell:
    """One labelled slice of the run, delimited by two ``source=mark`` rows."""

    label: str
    gaps_moving: list[float] = field(default_factory=list)
    gaps_still: list[float] = field(default_factory=list)
    rows: int = 0
    failures: int = 0
    plan_rows: int = 0
    persp_rows: int = 0
    # ---- the desync measurement's two streams (PLAT-RE84) ------------------
    # `archicad_samples` is ground truth: (t_ms, cx, cy, halfHeight, rotation).
    # `viewer_rows` is what was actually PRESENTED, plus the surface height that
    # turns a model error into pixels: (t_ms, cx, cy, halfHeight, rotation, h).
    archicad_samples: list = field(default_factory=list)
    viewer_rows: list = field(default_factory=list)
    # Milliseconds spent inside the ACAPI read, per sample (PLAT-RE87). This is
    # the half of the poll interval we control; the rest is Windows deciding when
    # to run us.
    read_us: list = field(default_factory=list)
    # (t_ms, swapchain) for every frame the DXGI hook saw presented, ours and
    # Archicad's alike -- one vtable serves the whole process.
    present_rows: list = field(default_factory=list)
    # The 3D pair (PLAT-RE101): (t, eye, target, coneDeg) for Archicad and
    # (t, eye, target, coneDeg, w, h) for the frames we presented.
    archicad_persp: list = field(default_factory=list)
    viewer_persp: list = field(default_factory=list)

    @property
    def all_gaps(self) -> list[float]:
        return self.gaps_moving + self.gaps_still


def percentile(values: list[float], fraction: float) -> float:
    """Nearest-rank percentile.  No numpy: this must run from a bare checkout."""
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, min(len(ordered), int(round(fraction * len(ordered) + 0.5))))
    return ordered[rank - 1]


def camera_signature(row: dict[str, str]) -> tuple[float, ...]:
    """The fields that change when Archicad's camera moves.

    Covers both modes at once: for ``persp`` these are eye and target, for
    ``plan`` they are the centre plus the half-height and rotation that reuse the
    ``dist``/``azimuth_deg`` columns (see NavLog.cpp's header block).
    """
    keys = ("eyeX", "eyeY", "eyeZ", "tgtX", "tgtY", "tgtZ", "dist", "azimuth_deg")
    out = []
    for key in keys:
        try:
            out.append(float(row.get(key, "0") or 0.0))
        except ValueError:
            out.append(0.0)
    return tuple(out)


def rotated_siblings(path: str) -> list[str]:
    """`path` preceded by its rotated predecessor, oldest first.

    ⚠️ A MATRIX RUN IS BIG ENOUGH TO ROTATE ITSELF, and the failure is silent.
    NavLog caps the file at 5 MiB and moves it to `<name>.1.log`; two matrix runs
    are about 5.6 MiB, so the rotation lands MID-RUN. The report then read only
    the tail and printed a confident four-cell table -- the twenty cells before
    the boundary simply were not there, and nothing said so. A truncated
    measurement that looks like a complete one is the worst thing this tool can
    do, so the predecessor is picked up automatically rather than on a flag
    nobody would know to pass.
    """
    stem, ext = os.path.splitext(path)
    previous = f"{stem}.1{ext}"
    return [previous, path] if os.path.isfile(previous) else [path]


def parse(paths) -> tuple[list[Cell], list[str]]:
    """Return the cells in file order, plus any notes worth printing.

    `paths` is one path or several, oldest first. Several are read as ONE stream
    so a cell straddling a rotation boundary stays a single cell rather than
    becoming two half-length ones.
    """
    if isinstance(paths, str):
        paths = [paths]
    notes: list[str] = []
    cells: list[Cell] = []
    # Rows before the first mark still matter -- a run started without the probe
    # has no marks at all and would otherwise report nothing.
    current = Cell(label="(unmarked)")
    previous_signature: tuple[float, ...] | None = None

    handles = [open(one, "r", encoding="utf-8", errors="replace") for one in paths]
    try:
        for line in itertools.chain(*handles):
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line.startswith("t_ms,"):
                if line.startswith("# throttle_ms="):
                    # ⚠️ A THROTTLE NEAR THE POLL INTERVAL ALIASES pollHz, and the
                    # aliased number reads as a starved timer. Two runs were
                    # analysed that way before the log said what its own throttle
                    # was.
                    try:
                        throttle = int(line.split("=", 1)[1])
                    except ValueError:
                        throttle = 0
                    if throttle >= 10:
                        notes.append(
                            f"WARNING: this session throttled the log at {throttle} ms, which is "
                            "close to the poll interval. Samples arriving even slightly early were "
                            "dropped BY THE LOG, so pollHz understates the real poll rate and must "
                            "not be read as a starved timer. Re-run: the matrix now asks for 0.")
                    continue
                if line.startswith("# ---- ArchViz navigation comparison, new session"):
                    # ⚠️ THE FILE IS APPENDED ACROSS SESSIONS.  Without this reset
                    # a cell label reused in two runs would pool their samples and
                    # average away exactly the regression being looked for.
                    if current.rows:
                        cells.append(current)
                    current = Cell(label="(unmarked)")
                    previous_signature = None
                    notes.append("session boundary: counters reset")
                continue

            fields = line.split(",")
            if len(fields) < len(COLUMNS) - 1:
                continue
            row = dict(zip(COLUMNS, fields))
            source = row["source"]

            if source == "mark":
                label = row.get("extra", "").strip()
                if current.rows:
                    cells.append(current)
                # ⚠️ A CLOSING MARK MUST NOT OPEN A CELL WITH THE SAME NAME.  It
                # did, and the first live run showed it: rows logged after the
                # last gesture ended -- while the probe was still tearing down --
                # landed in a SECOND "window resize" row with a plausible 253
                # samples in it.  Two rows with one name read as a flaky cell,
                # which is exactly the kind of thing this report exists to rule
                # out rather than manufacture.  What follows a `/end` is the gap
                # between cells, and it is named as such.
                if label.endswith("/end"):
                    current = Cell(label="(between cells)")
                else:
                    if label.endswith("/start"):
                        label = label[: -len("/start")]
                    current = Cell(label=label or "(unlabelled)")
                previous_signature = None
                continue

            if source == "viewer" and row["mode"] == "persp":
                width = height = 0
                for part in row.get("extra", "").split(";"):
                    if part.startswith("h="):
                        height = int(part[2:] or 0)
                    elif part.startswith("w="):
                        width = int(part[2:] or 0)
                try:
                    current.viewer_persp.append((
                        float(row["t_ms"]),
                        (float(row["eyeX"]), float(row["eyeY"]), float(row["eyeZ"])),
                        (float(row["tgtX"]), float(row["tgtY"]), float(row["tgtZ"])),
                        float(row["cone_or_fov_deg"]), width, height))
                except ValueError:
                    pass
                continue

            if source == "viewer" and row["mode"] == "plan":
                # The presented-frame stream. `extra` carries the surface size,
                # which can change mid-run when the window is resized -- reading
                # it per row rather than assuming one is what keeps the pixel
                # conversion honest across the `window resize` cell.
                try:
                    height = 0
                    for part in row.get("extra", "").split(";"):
                        if part.startswith("h="):
                            height = int(part[2:])
                    current.viewer_rows.append((
                        float(row["t_ms"]), float(row["tgtX"]), float(row["tgtY"]),
                        float(row["dist"]), math.radians(float(row["azimuth_deg"])),
                        height))
                except ValueError:
                    pass
                continue

            if source == "present":
                # Archicad's own frame clock (PLAT-RE78). Recorded by the DXGI
                # detour and flushed in batches, so these arrive out of band --
                # but they carry a session timestamp, aligned at flush time, so
                # they sit on the same timeline as the cameras.
                chain = 0
                ours = False
                # ⚠️ "WAS IT LABELLED" IS A SEPARATE FACT FROM "WAS IT OURS", and
                # conflating them mislabels every pre-RE91 row. Rows written
                # before the renderer stamped its own chain carry no `ours=` at
                # all; treating a missing key as False printed them as
                # "archicad", which is precisely the confident guess the label
                # was added to eliminate -- and it did it to a 59 Hz steady chain
                # that was almost certainly the overlay.
                labelled = False
                for part in row.get("extra", "").split(";"):
                    if part.startswith("chain="):
                        try:
                            chain = int(part[6:])
                        except ValueError:
                            pass
                    elif part.startswith("ours="):
                        labelled = True
                        ours = part[5:] == "1"
                try:
                    current.present_rows.append((float(row["t_ms"]), chain, ours, labelled))
                except ValueError:
                    pass
                continue

            if source != "archicad":
                continue

            current.rows += 1
            if row["mode"] == "fail":
                current.failures += 1
                continue
            if row["mode"] == "plan":
                current.plan_rows += 1
                try:
                    current.archicad_samples.append((
                        float(row["t_ms"]), float(row["tgtX"]), float(row["tgtY"]),
                        float(row["dist"]), math.radians(float(row["azimuth_deg"]))))
                except ValueError:
                    pass
                # How long the ACAPI read behind this row took. Absent in logs
                # written before PLAT-RE87, hence the quiet skip.
                for part in row.get("extra", "").split(";"):
                    if part.startswith("read_us="):
                        try:
                            current.read_us.append(float(part[8:]) / 1000.0)
                        except ValueError:
                            pass
            elif row["mode"] == "persp":
                current.persp_rows += 1
                try:
                    current.archicad_persp.append((
                        float(row["t_ms"]),
                        (float(row["eyeX"]), float(row["eyeY"]), float(row["eyeZ"])),
                        (float(row["tgtX"]), float(row["tgtY"]), float(row["tgtZ"])),
                        float(row["cone_or_fov_deg"])))
                except ValueError:
                    pass

            try:
                gap = float(row["gap_ms"])
            except ValueError:
                continue
            # gap==1 is NavLog's "first row of the stream" sentinel, not a 1 ms
            # poll; counting it would drag every percentile down.
            if gap <= 1.0:
                previous_signature = camera_signature(row)
                continue

            signature = camera_signature(row)
            moved = previous_signature is not None and any(
                abs(a - b) > MOVED_EPSILON for a, b in zip(signature, previous_signature)
            )
            previous_signature = signature
            (current.gaps_moving if moved else current.gaps_still).append(gap)
    finally:
        for handle in handles:
            handle.close()

    if current.rows:
        cells.append(current)
    return cells, notes


def report(cells: list[Cell], notes: list[str], as_csv: bool) -> None:
    if as_csv:
        print("cell,rows,plan,persp,fails,"
              "moving_n,moving_p50,moving_p95,moving_max,"
              "still_n,still_p50,still_p95,still_max")
        for cell in cells:
            print(",".join(str(x) for x in (
                cell.label.replace(",", ";"), cell.rows, cell.plan_rows, cell.persp_rows,
                cell.failures,
                len(cell.gaps_moving), percentile(cell.gaps_moving, 0.50),
                percentile(cell.gaps_moving, 0.95), max(cell.gaps_moving, default=0.0),
                len(cell.gaps_still), percentile(cell.gaps_still, 0.50),
                percentile(cell.gaps_still, 0.95), max(cell.gaps_still, default=0.0),
            )))
        return

    header = (f"{'cell':<28} {'rows':>5} {'fail':>4} | "
              f"{'MOVING n':>8} {'p50':>6} {'p95':>6} {'max':>6} | "
              f"{'STILL n':>8} {'p50':>6} {'p95':>6} {'max':>6}")
    print(header)
    print("-" * len(header))
    for cell in cells:
        print(f"{cell.label[:28]:<28} {cell.rows:>5} {cell.failures:>4} | "
              f"{len(cell.gaps_moving):>8} "
              f"{percentile(cell.gaps_moving, 0.50):>6.0f} "
              f"{percentile(cell.gaps_moving, 0.95):>6.0f} "
              f"{max(cell.gaps_moving, default=0.0):>6.0f} | "
              f"{len(cell.gaps_still):>8} "
              f"{percentile(cell.gaps_still, 0.50):>6.0f} "
              f"{percentile(cell.gaps_still, 0.95):>6.0f} "
              f"{max(cell.gaps_still, default=0.0):>6.0f}")

    for note in notes:
        print(f"note: {note}")

    print()
    print("MOVING = consecutive samples where Archicad's camera changed, i.e. the")
    print("user was navigating.  Those are the gaps that decide the ladder; the")
    print("STILL columns are there so a healthy-looking total cannot hide them.")
    print()
    print("Read it as:  MOVING p95 well under one frame (~16 ms at 60 Hz) means the")
    print("sample stream is clean -- a bad eye verdict then is structural, and the")
    print("answer is prediction (PLAT-RE76/RE80) or compositing (PLAT-RE79), not a")
    print("faster poll.  MOVING p95 far above it means the poll is starved and the")
    print("wake signal (PLAT-RE75) comes first.")


def default_log_path() -> str:
    root = os.environ.get("LOCALAPPDATA", "")
    return os.path.join(root, "Tapioca", "logs", "archviz_nav.log")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", default=default_log_path(),
                        help="path to archviz_nav.log (default: the live one)")
    parser.add_argument("--csv", action="store_true", help="machine-readable output")
    parser.add_argument("--desync-only", action="store_true",
                        help="skip the poll-gap table and print only the objective desync")
    args = parser.parse_args()

    if not os.path.isfile(args.log):
        # ⚠️ NAMES THE COMMAND THAT CREATES IT.  The log is opt-in and off by
        # default, so "missing" almost always means nobody turned it on -- and
        # the command that does was itself missing until PLAT-RE73, which is how
        # the 2026-08-06 samples came to have no successor run.
        print(f"no nav log at {args.log}", file=sys.stderr)
        print("turn it on in Archicad first: Tapioca.ViewerNavLog "
              "{enable: true, intervalMs: 16}", file=sys.stderr)
        return 1

    logs = rotated_siblings(args.log)
    if len(logs) > 1:
        print(f"reading {len(logs)} files (the log rotated mid-run): "
              + ", ".join(os.path.basename(one) for one in logs))
    cells, notes = parse(logs)
    if not cells:
        print(f"{args.log} has no archicad rows -- was the log running?", file=sys.stderr)
        return 1
    if not args.desync_only:
        report(cells, notes, args.csv)
    elif not args.csv:
        # ⚠️ THE WARNINGS BELONG WHEREVER THE NUMBERS ARE READ. --desync-only is
        # the form actually used to make decisions, and a caveat printed only in
        # the other form is a caveat nobody sees.
        # Warnings only. "session boundary" is bookkeeping and repeats per run;
        # burying one real warning under six of those is how it goes unread.
        for note in dict.fromkeys(n for n in notes if n.startswith("WARNING")):
            print(f"note: {note}")
    desync_report(cells, args.csv)
    if not args.csv:
        frame_clock_report(cells)
    return 0




# ---------------------------------------------------------------------------
# DESYNC (PLAT-RE84) -- the objective number.
#
# WHAT IT MEASURES.  Two streams share one clock: what ARCHICAD's camera was
# (`source=archicad, mode=plan`, sampled by the main-thread poll) and what the
# overlay actually PRESENTED (`source=viewer, mode=plan`, logged by the render
# thread after Present).  For each presented frame, Archicad's camera is
# interpolated to that frame's timestamp and the difference is reported in
# PIXELS of that frame's own view.
#
# ⚠️ PIXELS, NOT METRES.  A metre of error means nothing without the zoom -- the
# same metre is invisible on a site plan and enormous on a door detail.  Pixels
# are what the eye judges, which is what makes this comparable to the by-eye
# verdicts and comparable BETWEEN modes.
#
# ⚠️ IT ALSO RECOVERS THE LATENCY.  Sliding the presented stream forward in time
# and re-measuring finds the shift that minimises the error.  That shift IS the
# overlay's effective lag in milliseconds, measured rather than inferred -- and
# the error REMAINING at that shift is the part lag does not explain (jitter, a
# bad sample, prediction overshoot).  Two numbers that separate the two failure
# modes, which a single "it trails" cannot.
#
# ⚠️ WHAT IT DOES NOT MEASURE, AND THIS MATTERS BEFORE ANYONE QUOTES IT.  This is
# CAMERA desync, not photographed-pixel desync.  It cannot see the extra frame
# that DWM composition can add between two swap chains, so it is a LOWER BOUND on
# what the eye sees.  Closing that last gap needs the frame clock (PLAT-RE78) or
# a screen capture; this needs neither and can run on every session.

# ⚠️ NO GROUND TRUTH IS INVENTED ACROSS A GAP THIS LONG. A straight line between
# two samples 200 ms apart is a fiction, and it is worst exactly where the
# interesting cells are: an abrupt stop or a reversal inside the gap is smoothed
# into steady motion, so the overlay is compared against a camera Archicad never
# had. Three poll intervals at the measured ~60 Hz.
_MAX_INTERPOLATION_GAP_MS = 50.0


def _interpolate(samples, t):
    """Archicad's camera at time `t`, linearly between the two nearest samples.

    `samples` is a time-sorted list of (t, cx, cy, half, rot).  Returns None
    outside the sampled range -- extrapolating past the ends would invent
    exactly the motion the measurement is trying to detect -- and None across a
    gap longer than `_MAX_INTERPOLATION_GAP_MS`, for the same reason.
    """
    if len(samples) < 2 or t < samples[0][0] or t > samples[-1][0]:
        return None
    low, high = 0, len(samples) - 1
    while high - low > 1:
        mid = (low + high) // 2
        if samples[mid][0] <= t:
            low = mid
        else:
            high = mid
    t0, x0, y0, h0, r0 = samples[low]
    t1, x1, y1, h1, r1 = samples[high]
    span = t1 - t0
    if span > _MAX_INTERPOLATION_GAP_MS:
        return None
    if span <= 0:
        return samples[low][1:]
    f = (t - t0) / span
    # ⚠️ THE ROTATION IS INTERPOLATED ALONG THE SHORTEST ARC. A plain lerp between
    # 179 deg and -179 deg passes through ZERO -- half a turn of motion invented
    # between two samples two degrees apart -- and the frame in between is then
    # scored against a camera pointing the opposite way. Caught by the angle-wrap
    # fixture, which reported 2221 px of error for one degree of real rotation.
    return (x0 + (x1 - x0) * f,
            y0 + (y1 - y0) * f,
            h0 + (h1 - h0) * f,
            r0 + _wrap(r1 - r0) * f)


def _desync_pixels(viewer, archicad, shift_ms):
    """Per-frame desync in pixels, with the presented stream shifted in time.

    A POSITIVE `shift_ms` asks "what if the overlay's frame actually corresponds
    to Archicad's state `shift_ms` EARLIER" -- i.e. the overlay is behind.  The
    shift that minimises the error is therefore the lag.
    """
    out = []
    for t, cx, cy, half, rot, height in viewer:
        truth = _interpolate(archicad, t - shift_ms)
        if truth is None or height < 2 or half <= 0.0:
            continue
        tx, ty, thalf, trot = truth
        metres_per_pixel = half * 2.0 / float(height)
        if metres_per_pixel <= 0.0:
            continue
        offset = math.hypot(cx - tx, cy - ty) / metres_per_pixel
        # A zoom error is a scale error: express it as the pixel displacement it
        # causes at the edge of the view, which is where it is most visible and
        # is the honest worst case.
        zoom = abs(half - thalf) / metres_per_pixel
        # A rotation error displaces the corner by angle * radius.
        radius = math.hypot(float(height), float(height)) * 0.5
        spin = abs(_wrap(rot - trot)) * radius
        out.append((offset, zoom, spin, offset + zoom + spin))
    return out


def _dominant(rows):
    """Which of the three errors carries the cell: `pan`, `zoom` or `spin`.

    ⚠️ THE THREE COMPONENTS WERE ALWAYS COMPUTED AND NEVER REPORTED, so every
    verdict so far said "it trails" without saying trails IN WHAT. Three frames
    of a real wheel zoom (2026-08-14) show the overlay's squares both offset AND
    visibly larger than Archicad's -- a scale error, which needs a different fix
    from a position error and which the single desync number cannot distinguish
    from one. Reported at p95 rather than the median because a cell is judged on
    its worst moments; the share says whether it dominates or merely leads.
    """
    if not rows:
        return ""
    components = (("pan", percentile(sorted(r[0] for r in rows), 0.95)),
                  ("zoom", percentile(sorted(r[1] for r in rows), 0.95)),
                  ("spin", percentile(sorted(r[2] for r in rows), 0.95)))
    total = sum(value for _, value in components)
    if total <= 0.0:
        return "-"
    name, value = max(components, key=lambda pair: pair[1])
    return "%s %d%%" % (name, round(100.0 * value / total))


# ⚠️ THE PARAMETER SUM IS AN UPPER BOUND, NOT A DISPLACEMENT. `_desync_pixels`
# adds a translation error, a zoom error measured at the view edge and a rotation
# error measured at the corner -- three maxima that need not occur at the same
# point, so no single pixel is ever that far out. It is comparable between runs
# and it is what every number so far was quoted in, so it stays.
#
# This is the defensible version: take a grid of points spanning the view, put
# each through BOTH cameras, and report how far the same model point actually
# lands apart on screen. That is a displacement a user could measure with a
# ruler.
_GRID = tuple((gx / 2.0 - 1.0, gy / 2.0 - 1.0)
              for gx in range(5) for gy in range(5))   # -1..1 in both axes


def _plan_grid_pixels(viewer_row, truth):
    """Worst on-screen displacement of a shared model point, in pixels."""
    t, cx, cy, half, rot, height = viewer_row
    tx, ty, thalf, trot = truth
    if height < 2 or half <= 0.0 or thalf <= 0.0:
        return None
    # Screen pixels per metre, for each camera's own zoom.
    scale = (height / 2.0) / half
    worst = 0.0
    for ux, uy in _GRID:
        # A point on the TRUTH camera's view, in model space...
        rx = ux * thalf
        ry = uy * thalf
        cos_t, sin_t = math.cos(trot), math.sin(trot)
        mx = tx + rx * cos_t - ry * sin_t
        my = ty + rx * sin_t + ry * cos_t
        # ...projected through the camera the frame actually carried.
        dx = mx - cx
        dy = my - cy
        cos_v, sin_v = math.cos(-rot), math.sin(-rot)
        vx = (dx * cos_v - dy * sin_v) * scale
        vy = (dx * sin_v + dy * cos_v) * scale
        # Where the truth camera puts it on ITS screen.
        truth_scale = (height / 2.0) / thalf
        ex = ux * thalf * truth_scale
        ey = uy * thalf * truth_scale
        worst = max(worst, math.hypot(vx - ex, vy - ey))
    return worst


def _grid_desync(viewer, archicad, shift_ms):
    out = []
    for row in viewer:
        truth = _interpolate(archicad, row[0] - shift_ms)
        if truth is None:
            continue
        displaced = _plan_grid_pixels(row, truth)
        if displaced is not None:
            out.append(displaced)
    return out


# ---------------------------------------------------------------------------
# 3D scoring (PLAT-RE101/RE105).
#
# ⚠️ UNTIL NOW A `path=3d` RUN COULD NOT BE SCORED AT ALL. The renderer rejected
# perspective cameras when logging presented frames, so a 3D run recorded
# Archicad's camera and nothing to compare it against -- and the report only ever
# parsed `mode=plan` viewer rows anyway. Every 3D verdict has been by eye, and
# nothing said so.
#
# The measure is the same idea as the plan grid: take reference points, put them
# through BOTH cameras, and report how far apart the same point lands on screen.
# The points are taken on the truth camera's image plane at target distance and
# back-projected, so they span what the user is actually looking at rather than
# an arbitrary box.

def _normalise(v):
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if length <= 1e-12:
        return None
    return (v[0] / length, v[1] / length, v[2] / length)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _basis(eye, target):
    """Right/up/forward for a Z-up camera looking from `eye` at `target`."""
    forward = _normalise((target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]))
    if forward is None:
        return None
    right = _normalise(_cross(forward, (0.0, 0.0, 1.0)))
    if right is None:      # looking straight down: any horizontal right will do
        right = (1.0, 0.0, 0.0)
    up = _cross(right, forward)
    return right, up, forward


def _project(point, eye, basis, focal_px, width, height):
    """Model point -> pixel, or None if it is behind the camera."""
    right, up, forward = basis
    d = (point[0] - eye[0], point[1] - eye[1], point[2] - eye[2])
    z = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2]
    if z <= 1e-6:
        return None
    x = d[0] * right[0] + d[1] * right[1] + d[2] * right[2]
    y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2]
    return (width / 2.0 + focal_px * x / z, height / 2.0 - focal_px * y / z)


def _persp_grid_pixels(viewer_row, truth):
    """Worst on-screen displacement of a shared model point, in pixels."""
    _t, veye, vtarget, vcone, width, height = viewer_row
    teye, ttarget, tcone = truth
    if width < 2 or height < 2 or vcone <= 0.0 or tcone <= 0.0:
        return None
    vbasis = _basis(veye, vtarget)
    tbasis = _basis(teye, ttarget)
    if vbasis is None or tbasis is None:
        return None

    # Focal length in pixels from the HORIZONTAL cone -- the convention both
    # streams are logged in. Mixing a horizontal cone with a vertical focal
    # length is a systematic error that would look like a constant zoom offset.
    vfocal = (width / 2.0) / math.tan(math.radians(vcone) / 2.0)
    tfocal = (width / 2.0) / math.tan(math.radians(tcone) / 2.0)

    distance = math.sqrt(sum((ttarget[i] - teye[i]) ** 2 for i in range(3)))
    if distance <= 1e-6:
        return None
    tright, tup, tforward = tbasis

    worst = 0.0
    for ux, uy in _GRID:
        # A point on the truth camera's image plane at target distance.
        sx = ux * (width / 2.0) / tfocal * distance
        sy = uy * (height / 2.0) / tfocal * distance
        point = tuple(teye[i] + tforward[i] * distance + tright[i] * sx + tup[i] * sy
                      for i in range(3))
        a = _project(point, teye, tbasis, tfocal, width, height)
        b = _project(point, veye, vbasis, vfocal, width, height)
        if a is None or b is None:
            continue
        worst = max(worst, math.hypot(a[0] - b[0], a[1] - b[1]))
    return worst


def _interpolate_persp(samples, t):
    """Archicad's 3D camera at `t`: (eye, target, coneDeg). Same gap rule."""
    if len(samples) < 2 or t < samples[0][0] or t > samples[-1][0]:
        return None
    low, high = 0, len(samples) - 1
    while high - low > 1:
        mid = (low + high) // 2
        if samples[mid][0] <= t:
            low = mid
        else:
            high = mid
    t0, eye0, tgt0, cone0 = samples[low]
    t1, eye1, tgt1, cone1 = samples[high]
    span = t1 - t0
    if span > _MAX_INTERPOLATION_GAP_MS:
        return None
    if span <= 0:
        return (eye0, tgt0, cone0)
    f = (t - t0) / span
    lerp3 = lambda a, b: tuple(a[i] + (b[i] - a[i]) * f for i in range(3))
    return (lerp3(eye0, eye1), lerp3(tgt0, tgt1), cone0 + (cone1 - cone0) * f)


def _wrap(radians):
    while radians > math.pi:
        radians -= 2.0 * math.pi
    while radians < -math.pi:
        radians += 2.0 * math.pi
    return radians


def _rms(values):
    if not values:
        return float("inf")
    return math.sqrt(sum(v * v for v in values) / len(values))


def best_fit_latency(viewer, archicad, max_shift_ms=200.0, step_ms=2.0):
    """The time shift that best explains the desync, and the residual there.

    ⚠️ IT SEARCHES NEGATIVE SHIFTS TOO.  An overlay that PREDICTS can legitimately
    be AHEAD of Archicad, and a search that only looked forward would report its
    lag as zero and its residual as large -- reading as "jittery" when it is
    actually early.  That distinction is the entire point of comparing `predict`
    against `legacy`.
    """
    # ⚠️ EVERY SHIFT IS SCORED OVER THE SAME FRAMES, AND THIS IS NOT A DETAIL.
    # A frame is only measurable if the shifted timestamp lands inside the
    # ground-truth stream, so each shift used to drop a DIFFERENT set of frames
    # at the ends and across gaps -- and a shift could then win simply because
    # the hardest frames had fallen out of its support, not because it aligned
    # anything. That biases the fit toward large shifts, which is exactly the
    # direction the predict modes are being judged in.
    #
    # So: find the frames measurable at EVERY candidate shift first, and score
    # only those.
    support = None
    shift = -max_shift_ms
    while shift <= max_shift_ms:
        measurable = {t for t, _cx, _cy, _half, _rot, _h in viewer
                      if _interpolate(archicad, t - shift) is not None}
        support = measurable if support is None else (support & measurable)
        shift += step_ms
    if not support:
        return (0.0, float("inf"))

    common = [row for row in viewer if row[0] in support]
    best = (0.0, float("inf"))
    shift = -max_shift_ms
    while shift <= max_shift_ms:
        rows = _desync_pixels(common, archicad, shift)
        if rows:
            residual = _rms([r[3] for r in rows])
            if residual < best[1]:
                best = (shift, residual)
        shift += step_ms
    return best


# Below this, shifting the streams in time changes nothing measurable, so the
# fit has nothing to grip.
_STILL_PIXELS = 1.0


def fit_quality(viewer, archicad, lag, max_shift_ms=200.0, step_ms=2.0):
    """Whether the recovered lag means anything.  "ok" | "still" | "saturated".

    ⚠️ A LAG NUMBER IS NOT SELF-VALIDATING, and the first real run proved it: every
    idle cell reported -200 ms and one reported +200, which are simply the two ends
    of the search.  When the camera does not move, EVERY shift scores zero error and
    the search returns whichever it tried first -- a confident, meaningless number
    sitting in the same column as the real ones.  Same at the boundary: a fit that
    lands on the last shift tried means the true lag is somewhere past the window,
    not that it is exactly 200 ms.
    """
    worst = 0.0
    shift = -max_shift_ms
    while shift <= max_shift_ms:
        rows = _desync_pixels(viewer, archicad, shift)
        if rows:
            worst = max(worst, _rms([r[3] for r in rows]))
        shift += step_ms
    # A fit with no common support is not "still" -- it is unmeasurable, and
    # saying "still" would claim the camera did not move.
    if worst < _STILL_PIXELS:
        return "still"
    if abs(abs(lag) - max_shift_ms) < step_ms:
        return "saturated"
    return "ok"


def truth_hz(archicad):
    """How often the ground-truth poll actually produced a sample.

    ⚠️ THE POLL IS THE CEILING, and this column is what makes that visible. The
    overlay cannot be fresher than the samples driving it, so a cell showing 30 Hz
    here has a 33 ms floor under its desync no matter what the render thread does.
    Without this next to the pixels, a starved poll reads as a slow renderer.
    """
    if len(archicad) < 2:
        return 0.0
    span = archicad[-1][0] - archicad[0][0]
    if span <= 0:
        return 0.0
    return (len(archicad) - 1) * 1000.0 / span


def frame_clock_report(cells):
    """Archicad's own present cadence, from the DXGI hook (PLAT-RE78).

    ⚠️ THIS IS THE ONLY GROUND TRUTH ABOUT FRAMES IN THE WHOLE TOOLCHAIN. Every
    other number here is measured against OUR poll, so the best it can say is how
    stale the overlay is relative to a sample -- it cannot say how many of
    Archicad's frames went out in between, which is what "one frame behind"
    actually means. The lag sat at 22-28 ms across `legacy`, `wake` and `predict`
    alike while the ACAPI read cost 0.1 ms; whether that is two frames of a 60 Hz
    cadence or one frame of a 30 Hz one changes which rung comes next.
    """
    rows = []
    for cell in cells:
        rows.extend(cell.present_rows)
    if not rows:
        return

    by_chain = {}
    owned = {}
    labelled_chains = set()
    for t, chain, ours, labelled in rows:
        by_chain.setdefault(chain, []).append(t)
        owned[chain] = owned.get(chain, False) or ours
        if labelled:
            labelled_chains.add(chain)

    print()
    header = (f"{'swap chain':>18} {'whose':>9} {'frames':>7} {'medianms':>9} "
              f"{'p95ms':>7} {'impliedHz':>10}")
    print(header)
    print("-" * len(header))
    for chain, times in sorted(by_chain.items(), key=lambda kv: -len(kv[1])):
        times.sort()
        deltas = [b - a for a, b in zip(times, times[1:]) if b > a]
        if len(deltas) < 2:
            continue
        deltas.sort()
        median = percentile(deltas, 0.50)
        hz = (1000.0 / median) if median > 0 else 0.0
        # `?` means THIS CHAIN was never labelled -- a pre-RE91 session -- not
        # that it is Archicad's. Unknown has to read as unknown.
        if chain not in labelled_chains:
            whose = "?"
        else:
            whose = "OVERLAY" if owned.get(chain) else "archicad"
        print(f"{chain:>18} {whose:>9} {len(times):>7} {median:>9.1f} "
              f"{percentile(deltas, 0.95):>7.1f} {hz:>10.0f}")

    print()
    print("whose = OVERLAY is ours, reported by the renderer itself rather than")
    print("inferred; anything else on this list is Archicad's. Compare ARCHICAD's")
    print("median frame time against the measured lag:")
    print("  lag ~= 2x frame time -> pipeline depth, which prediction can cancel")
    print("     once its horizon is the MEASURED frame time instead of a constant.")
    print("  lag ~= 1x frame time and immovable -> the DWM composition race, which")
    print("     only compositing into Archicad's own back buffer (RE79) removes.")
    print("  lag ~= one POLL interval -> neither: we are simply one sample behind,")
    print("     and the fix is a faster sampler or prediction, not a frame trick.")


def desync_report(cells, as_csv):
    """Print the objective table: pixels of desync, and the latency behind it."""
    usable = [c for c in cells if (c.viewer_rows and c.archicad_samples)
              or (c.viewer_persp and c.archicad_persp)]
    if not usable:
        print()
        print("NO DESYNC MEASUREMENT: the log has no `source=viewer, mode=plan` rows.")
        print("Those come from the render thread after Present, and they only exist")
        print("when the nav log was running WHILE an overlay was up. Re-run the matrix")
        print("with the overlay open over the floor plan.")
        return

    if as_csv:
        print("cell,frames,truth_hz,px_p50,px_p95,px_max,lag_ms,residual_px,fit")
    else:
        header = (f"{'cell':<28} {'frames':>6} {'pollHz':>6} {'readms':>6} | {'p50':>5} "
                  f"{'p95':>6} {'max':>6} | {'GRIDp50':>7} {'GRIDp95':>7} | "
                  f"{'lag ms':>7} {'resid':>6}  {'fit':<9} worst")
        print()
        print(header)
        print("-" * len(header))

    # ⚠️ hookdraw CELLS ARE NOT REPORTABLE BY THIS MEASUREMENT, AND SAYING SO IS
    # THE ONLY HONEST OPTION. The viewer rows are timestamped after OUR swap
    # chain presents (DiligentViewportSupport -> LogPresentedPlanFrame), but in
    # hookdraw that frame is not what reaches the screen: the host compositor
    # picks it up on some LATER Archicad Present, or reuses an older one, and
    # nothing associates a producer frame with the host Present that consumed
    # it. So the numbers below describe the hidden producer stream, not the
    # image the user saw -- close enough to look right, which is what makes it
    # dangerous. Until each shared frame is stamped and its consuming Present
    # logged, these cells are listed and left unscored.
    hookdraw = [cell for cell in usable if "hookdraw" in cell.label]
    if hookdraw:
        print("!! %d hookdraw cell(s) are NOT SCORED below. In that mode the frame this"
              % len(hookdraw))
        print("   report timestamps is the one OUR swap chain presented, and the host")
        print("   compositor consumes it on a later Archicad Present or reuses an older")
        print("   one. The desync would describe the hidden producer stream, not the")
        print("   picture on screen. Judge hookdraw by eye until the shared frames carry")
        print("   a generation and their consuming Present is logged.")
        print()
        for cell in hookdraw:
            print(f"{cell.label[:28]:<28} {'-- not scored (hookdraw) --':>60}")
        print()
    usable = [cell for cell in usable if "hookdraw" not in cell.label]

    for cell in usable:
        # ⚠️ 3D IS SCORED BY PROJECTION, NOT BY THE PLAN FORMULA. There is no
        # half-height and no plan rotation in a perspective camera, and reusing
        # the plan columns would produce confident nonsense. A 3D cell reports
        # only the grid displacement -- the parameter sum has no meaning here.
        if cell.viewer_persp and cell.archicad_persp:
            grid = [g for g in (_persp_grid_pixels(row, _interpolate_persp(
                        cell.archicad_persp, row[0]))
                    for row in cell.viewer_persp
                    if _interpolate_persp(cell.archicad_persp, row[0]) is not None)
                    if g is not None]
            if not grid:
                continue
            grid.sort()
            hz3 = truth_hz([(t, 0, 0, 0, 0) for t, *_rest in cell.archicad_persp])
            if as_csv:
                print("%s,%d,%.1f,,,,%.2f,%.2f,,,3d" % (
                    cell.label.replace(",", ";"), len(grid), hz3,
                    percentile(grid, 0.50), percentile(grid, 0.95)))
            else:
                print(f"{cell.label[:28]:<28} {len(grid):>6} {hz3:>6.0f} {'-':>6} | "
                      f"{'-':>5} {'-':>6} {'-':>6} | "
                      f"{percentile(grid, 0.50):>7.1f} {percentile(grid, 0.95):>7.1f} | "
                      f"{'--':>7} {'-':>6}  3d-grid")
            continue

        rows = _desync_pixels(cell.viewer_rows, cell.archicad_samples, 0.0)
        if not rows:
            continue
        totals = sorted(r[3] for r in rows)
        grid = sorted(_grid_desync(cell.viewer_rows, cell.archicad_samples, 0.0))
        lag, residual = best_fit_latency(cell.viewer_rows, cell.archicad_samples)
        quality = fit_quality(cell.viewer_rows, cell.archicad_samples, lag)
        hz = truth_hz(cell.archicad_samples)
        # A lag the fit cannot stand behind is not printed as a number. Printing
        # it with a footnote was not enough -- it still got read off the column.
        lag_text = f"{lag:>7.0f}" if quality == "ok" else f"{'--':>7}"
        if as_csv:
            print("%s,%d,%.1f,%.2f,%.2f,%.2f,%s,%.2f,%s" % (
                cell.label.replace(",", ";"), len(rows), hz,
                percentile(totals, 0.50), percentile(totals, 0.95), totals[-1],
                ("%.1f" % lag) if quality == "ok" else "", residual, quality))
        else:
            read = (f"{percentile(sorted(cell.read_us), 0.50):>6.1f}"
                    if cell.read_us else f"{'-':>6}")
            gp50 = percentile(grid, 0.50) if grid else 0.0
            gp95 = percentile(grid, 0.95) if grid else 0.0
            print(f"{cell.label[:28]:<28} {len(rows):>6} {hz:>6.0f} {read} | "
                  f"{percentile(totals, 0.50):>5.1f} {percentile(totals, 0.95):>6.1f} "
                  f"{totals[-1]:>6.1f} | {gp50:>7.1f} {gp95:>7.1f} | "
                  f"{lag_text} {residual:>6.1f}  {quality:<9} {_dominant(rows)}")

    if as_csv:
        return
    print()
    print("DESYNC px = how far the overlay was from Archicad, in pixels of the view,")
    print("at the moment each frame was PRESENTED. This is the objective version of")
    print('"it trails": 0-1 px is indistinguishable, 2-5 px is visible on an edge,')
    print("20+ px is the picture sliding off the model.")
    print()
    print("lag ms = the time shift that best explains it. POSITIVE means the overlay")
    print("is BEHIND Archicad; NEGATIVE means it is AHEAD, which is what a working")
    print("`predict` mode should look like. residual px = the error still left after")
    print("that shift -- the part lag does NOT explain: jitter, a bad sample, or")
    print("prediction overshoot.")
    print()
    print("fit = whether the lag means anything. `still` = the camera barely moved,")
    print("so every shift scores the same and there is nothing to fit. `saturated` =")
    print("the best shift was the last one tried, so the true lag is past the search")
    print("window. Only `ok` rows have a readable lag; the rest print `--`.")
    print()
    print("GRID = the defensible error: a grid of model points put through BOTH")
    print("cameras, reporting how far the SAME point lands apart on screen. The p50/")
    print("p95/max columns to its left are a parameter SUM (translation + zoom at the")
    print("edge + rotation at the corner) -- an upper bound, since those three maxima")
    print("need not occur at the same point. Both are shown because every number")
    print("recorded before 2026-08-14 is in the old units; prefer GRID for new work.")
    print()
    print("worst = which of the three errors carries the cell at p95, and its")
    print("share of the total: `pan` is a position error, `zoom` a scale error and")
    print("`spin` a rotation error. They need different fixes, and the single")
    print("desync number cannot tell them apart -- a zoom-dominated cell means the")
    print("overlay is drawing the view at the WRONG SIZE, not merely in the wrong")
    print("place, which is what a wheel zoom's animation produces.")
    print()
    print("A `3d-grid` row is a 3D cell: only the projected displacement is")
    print("meaningful there, so the plan columns are blank rather than fabricated.")
    print()
    print("readms = median time inside the ACAPI read itself. It splits the poll")
    print("interval into the half we control and the half Windows controls: if readms")
    print("is most of 1000/pollHz the fix is to READ LESS, and no faster wake source")
    print("will help; if it is a small fraction, we are simply not being scheduled and")
    print("the wake source is exactly the thing to change.")
    print()
    print("pollHz = how often the ground-truth poll produced a sample IN THIS CELL.")
    print("It is the ceiling: the overlay cannot be fresher than the samples driving")
    print("it, so 30 Hz here puts a ~33 ms floor under the lag whatever the renderer")
    print("does. A cell whose pollHz collapses under drag is a starved poll, not a")
    print("slow overlay -- and no amount of prediction fixes a sample that is missing.")
    print()
    # ASCII in PRINTED output, whatever the comments above use: this console is
    # cp1252 and a warning glyph raised UnicodeEncodeError *after* the table had
    # been printed -- so the run looked like it had crashed halfway through
    # producing the numbers, when the numbers were already complete.
    print("!! LOWER BOUND, TWICE OVER. This is CAMERA desync: it cannot see the extra")
    print("   frame DWM can add between two swap chains. And the ground truth IS the")
    print("   poll, so it measures the overlay against the samples -- not against what")
    print("   Archicad actually drew between them.")


if __name__ == "__main__":
    sys.exit(main())
