"""Layer 2 — feedback on screen, rendered by Archicad's own DG.

    evp.ui.status("Processing storey 3 of 8...")   # the palette's status line
    evp.ui.progress("Rendering", 3, 8)             # the same line, "[3/8] Rendering"
    evp.ui.stages([...])                           # a whole multi-stage run
    evp.ui.alert("Open the 3D window (F5), then run again.")   # modal, unmissable

Both are FIRE-AND-FORGET: they post to the main thread and return immediately
rather than waiting. That is deliberate, and it is not just an optimisation —
a modal alert does not return until the user dismisses it, so blocking on one
would hold the main-thread gate for human time and then look like a hang. It
also means status() is cheap enough to call inside a loop.

Principle 3 of the plan: all user-facing UI comes from the Archicad API. A script
never draws its own window; it asks, and C++ renders.

Choosing between them:
  status()  progress and running commentary. Costs nothing, easy to miss.
  alert()   something the user must ACT on before the result makes sense.
            Keep it SHORT — Archicad grows the dialog to fit its text, and a
            full transcript stretches it past the height of the screen. Put the
            detail in a log file and the instruction in the alert.

A command that runs for MINUTES has a third obligation neither of those covers:
saying WHERE IT IS and WHAT THE USER SHOULD DO while it works. Until it says so
the palette shows "Running X..." for the whole run, the log file it is really
writing to is invisible from the panel, and a probe that needs the plan window
left alone has no way to say it. progress()/stages() are that channel — see
their docstrings.
"""

import json as _json
import time as _time

from .api import call, Cancelled
from . import selection as _selection


def status(message):
    """Write one line to the palette's status area. Returns immediately."""
    call("Tapioca.SetStatus", {"message": str(message)}, raise_on_error=False)


# How often a progress line is allowed to reach the palette. A status line is
# READ BY A HUMAN — rewriting it faster than this cannot be followed anyway, and
# each rewrite is a bus call plus a gate Post. A stage CHANGE always goes through
# (see _emit): the throttle exists to survive per-item loops, not to swallow the
# few lines that carry the structure of the run.
_MIN_INTERVAL = 0.25
_last_emit = 0.0


def _emit(text, force):
    global _last_emit
    now = _time.monotonic()
    if not force and (now - _last_emit) < _MIN_INTERVAL:
        return False
    _last_emit = now
    status(text)
    return True


def progress(message, step=None, total=None, hint=None, force=False):
    """One progress line on the palette's status line. Fire-and-forget.

    `step`/`total` prefix it with the position — "[3/8] Rendering" — so the user
    can see both where the run is and how much is left. Either may be omitted;
    with neither, this is status() plus the throttle.

    `hint` is what the USER should do (or not do) right now: "leave the plan
    window in front", "this takes ~40s". It is appended after an em dash and, on
    a narrow palette, is the first thing to be clipped — so put the instruction
    in `message` when it matters more than the position.

    Throttled to ~4 lines a second so it is safe to call inside a loop; pass
    force=True for a line that must not be dropped (a stage boundary, the last
    line before a long silent phase). Returns True if the line was sent.
    """
    text = str(message)
    if step is not None and total:
        text = "[%s/%s] %s" % (step, total, text)
    elif step is not None:
        text = "[%s] %s" % (step, text)
    if hint:
        text = "%s — %s" % (text, hint)
    return _emit(text, force)


def overlay_instruction(message, seconds=None):
    """One big line across the top of the Diligent OVERLAY, with a countdown.

    ⚠️ THE ONLY CHANNEL THAT WORKS WHILE THE USER IS NAVIGATING. Archicad's DG
    palette does not repaint during a navigation drag, so status()/progress()
    are frozen for the whole gesture — which is exactly the interval a
    measurement run needs to talk during, and the log file is read long
    afterwards. The overlay renders every frame regardless, so its HUD is the
    one surface that can carry a live instruction.

    `seconds` shows a large countdown that ticks in REAL TIME and clears itself
    when it expires: the deadline lives on the render thread, so a caller busy
    in a sleep loop — or one that dies — cannot leave a stale banner or a frozen
    number on screen. Omit it for text with no countdown.

    Empty `message` hides the banner. No-op with no overlay running, so a
    command may call it unconditionally.
    """
    return call("Tapioca.SetOverlayInstruction",
                {"text": str(message),
                 "seconds": -1.0 if seconds is None else float(seconds)},
                raise_on_error=False).ok


class Stages:
    """The run's stage counter — see stages(), which is how you get one."""

    def __init__(self, names, title=None, echo=None):
        self.names = [str(n) for n in names]
        self.title = title
        self.echo = echo
        self.index = 0           # 0 == not started; 1-based once running
        self._finished = False

    @property
    def total(self):
        return len(self.names)

    def next(self, hint=None, label=None):
        """Advance to the next stage and show it. Returns the stage's label.

        `label` overrides the declared name when the real one is only known at
        run time ("Storey 3 of 8"); the position is still the declared one, so
        the user still sees how much is left.
        """
        if self.index < self.total:
            self.index += 1
        name = label or (self.names[self.index - 1] if self.index else "")
        self._show(name, hint, force=True)
        return name

    def note(self, message, hint=None):
        """Detail WITHIN the current stage — throttled, position preserved."""
        self._show(message, hint, force=False)

    def _show(self, name, hint, force):
        line = "%s: %s" % (self.title, name) if self.title else name
        progress(line, step=self.index or None, total=self.total,
                 hint=hint, force=force)
        if self.echo is not None and force:
            self.echo("[%d/%d] %s" % (self.index, self.total, name))

    def finish(self, summary=None):
        """Close the run out. Called for you when used as a context manager.

        `summary` is for a stage set that ends BEFORE the command does — the
        palette stamps its own "<command>: done" over the status line when run()
        returns, so a summary emitted on the last line of a command is written
        and immediately overwritten.
        """
        if self._finished:
            return
        self._finished = True
        if summary:
            progress(summary, force=True)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        # An exception owns the status line on the way out: the palette's own
        # FinishRun overwrites it with the failure a moment later, and a cheerful
        # "done" in between just flickers. Same for a cancel.
        if exc_type is None:
            self.finish()
        else:
            self._finished = True
        return False


def stages(names, title=None, echo=None):
    """Report a multi-stage run stage by stage. Returns a Stages (a context manager).

        with evp.ui.stages(["Read roofs", "Fit planes", "Place symbols"],
                           title="Slope symbols", echo=_say) as st:
            st.next()                       # [1/3] Slope symbols: Read roofs
            ...
            st.next(hint="do not touch the plan window")
            for i, roof in enumerate(roofs):
                st.note("roof %d of %d" % (i + 1, len(roofs)))   # throttled

    Declare the stages UP FRONT — the count is the honest part of the display,
    and a stage list invented as the run goes cannot show how much is left.

    `echo`, if given, is called with each stage line as well (pass a probe's own
    `_say`) so the log file and the palette tell the same story — the palette
    line is transient and the log is what the user reads afterwards.

    Nothing here blocks: every line goes out through status(), which posts to the
    main thread and returns.
    """
    return Stages(names, title=title, echo=echo)


# Named text colours for table rows, resolved to [r,g,b] here so the add-on never
# has to keep a parallel table. Tuned to read on both the light and dark palette.
TABLE_COLORS = {
    "red":    (200, 30, 30),
    "orange": (210, 110, 20),
    "amber":  (180, 130, 0),
    "green":  (30, 140, 40),
    "blue":   (30, 90, 200),
    "gray":   (120, 120, 120),
    "grey":   (120, 120, 120),
}


def _resolve_color(color):
    """None -> None; a name from TABLE_COLORS or an (r,g,b) 0-255 sequence -> [r,g,b]."""
    if color is None:
        return None
    if isinstance(color, str):
        rgb = TABLE_COLORS.get(color.strip().lower())
        if rgb is None:
            raise ValueError(
                "evp.ui.table: unknown colour %r. Use one of %s, or an (r,g,b) tuple."
                % (color, ", ".join(sorted(TABLE_COLORS)))
            )
        return list(rgb)
    r, g, b = color   # any 3-element sequence
    return [int(r) & 255, int(g) & 255, int(b) & 255]


def table(headers, rows, row_colors=None, row_ids=None):
    """Show a one-shot results table in the palette. Fire-and-forget.

    `headers` is a sequence of column titles; `rows` is a sequence of rows, each a
    sequence of cell values (anything str()-able). The table renders under the
    command's parameters and REPLACES whatever a previous call showed — it is a
    result display, not a running log. Like status()/alert() it posts to the main
    thread and returns immediately, so it never blocks the script.

    Column count follows `headers`; a row with fewer cells than headers leaves the
    trailing columns blank, and extra cells past the header count are dropped. When
    a row is wider than the panel the table scrolls horizontally rather than
    squeezing columns below a readable width.

    `row_colors`, if given, is a sequence parallel to `rows`; each entry colours
    that row's text to flag it — an outlier, a failure — and is either None (leave
    default), a name from TABLE_COLORS ("red", "orange", ...), or an (r,g,b) tuple
    with channels 0-255.

    `row_ids`, if given, is a sequence parallel to `rows` of element GUID strings
    (or None). A row that carries a GUID becomes clickable: a single click on it
    SELECTS that element in Archicad, immediately, with no need to re-run — so a
    table of e.g. slab areas doubles as a navigator back to the model. Pass the
    GUIDs a command already has from evp.selection.get() or evp.elements.

    Rows cross the bus as an array of JSON strings — the shape ObjectState reads
    back reliably on the C++ side (a bare nested array does not survive). Each row
    is wrapped as {"cells":[...], "rgb":[r,g,b], "guid":"..."} so the add-on can
    parse it with the same ConvertToObjectState path everything else uses.
    """
    def _row_json(index, row):
        cell = {"cells": [str(c) for c in row]}
        if row_colors is not None and index < len(row_colors):
            rgb = _resolve_color(row_colors[index])
            if rgb is not None:
                cell["rgb"] = rgb
        if row_ids is not None and index < len(row_ids) and row_ids[index]:
            cell["guid"] = str(row_ids[index])
        return _json.dumps(cell)

    payload = {
        "headers": [str(h) for h in headers],
        "rows": [_row_json(i, row) for i, row in enumerate(rows)],
    }
    call("Tapioca.ShowResults", payload, raise_on_error=False)


def text(value):
    """Show selectable plain-text results in the palette.

    The native read-only multi-line field supports normal mouse selection and
    Ctrl+C, including a partial selection. Each call replaces the previous report
    without introducing table columns or a custom clipboard path.
    """
    call("Tapioca.ShowResultText", {"text": str(value)}, raise_on_error=False)


def current_params():
    """Return the palette's current parameter values as a dict.

    Ordinary command parameters are a start-of-run snapshot. Use this only for a
    long-lived command whose interface explicitly promises live controls; each
    call crosses to the main thread to read DG safely.
    """
    result = call("Tapioca.GetCurrentParams", raise_on_error=False).data or {}
    try:
        return _json.loads(result.get("paramsJson", "{}"))
    except (TypeError, ValueError):
        return {}


def alert(message):
    """Pop a native modal alert. Returns immediately; the dialog outlives the call.

    Keep it to a couple of short lines — see the module docstring.
    """
    call("Tapioca.ShowAlert", {"message": str(message)}, raise_on_error=False)


def request_selection(message="Select the elements to work on, then press Continue.",
                      poll_interval=0.15, timeout=None):
    """Ask the user to select elements interactively, and return their GUIDs.

    THE NON-SEIZING RUNG. A live ACAPI_UserInput_* pick was measured to block the
    main-thread gate entirely (4/4), which would freeze the very event loop that
    dispatches this command's work — so this does NOT seize a pick. Instead the
    palette shows `message` and a Continue button, and this call POLLS (on the
    command's own worker thread, which is free to block) until the user presses it,
    then reads the selection back. The UI stays live the whole time.

    Returns the selected GUIDs (a list). Raises evp.Cancelled if the user closes
    the palette instead, or the palette is absent (no one could ever press
    Continue). `timeout` (seconds, or None to wait indefinitely) caps the wait and
    also raises Cancelled — a command must never be able to hang here forever.

    Blocks until the user acts, so unlike status()/alert() it is NOT fire-and-
    forget. Do not call it in a tight loop; it is a single interaction point.
    """
    call("Tapioca.ShowSelectionPrompt", {"message": str(message)}, raise_on_error=False)
    started = _time.monotonic()
    try:
        while True:
            state = call("Tapioca.PollSelectionPrompt").data or {}
            if state.get("continued"):
                return _selection.get()
            if state.get("cancelled"):
                raise Cancelled()
            if timeout is not None and (_time.monotonic() - started) > timeout:
                raise Cancelled()
            _time.sleep(poll_interval)
    finally:
        # Always leave the prompt state, even on cancel/timeout/exception, so the
        # Continue button never lingers into the next command.
        call("Tapioca.HideSelectionPrompt", raise_on_error=False)
