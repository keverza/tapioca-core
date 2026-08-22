"""The `ctx` a schema-style command receives — what it is, and where it writes.

    def run(ctx, inputs):
        ctx.say("reading %d slabs" % len(guids))
        ...
        ctx.flush()

A signature-style command never sees one; this exists because a command that
declares `inputs=` takes exactly `(ctx, inputs)`, and everything that used to
arrive as a loose parameter or a module-level global has to arrive somewhere.

`say`/`flush` are here because every command in the tree hand-rolls the same
pair — a module-level `_lines` list, a `_say` that prints and appends, a
`_flush` that writes the log — and each copy re-decides the log's name. One
implementation means one naming rule and one place the rotation cap applies.
"""

from __future__ import annotations

from . import paths
from . import preview as _preview

__all__ = ["Context"]


class Context:
    """What the runtime knows about this run, plus the command's log.

    ⚠️ A Context belongs to ONE run. It is created by `evp._invoke` per
    invocation and never cached: `say()` accumulates lines, so a reused context
    would write the previous run's output again.
    """

    __slots__ = ("name", "folder", "mode", "previous_plan", "plan", "scene",
                 "region", "_lines", "_log_name")

    def __init__(self, name, folder=None, mode="run", log_name=None,
                 previous_plan=None, preview_kind=None, region=""):
        self.name = name
        self.folder = folder
        # The Plan `evp._invoke` built from this run's inputs, before anything
        # was written. Use it rather than calling the planner again: a second
        # call after the writes would read a model that already contains them.
        # None when the command declares no plan=.
        self.plan = None
        # "run" performs the command. "preview" asks it for a Plan and commits
        # nothing — the palette's preview band. A command that branches on this
        # is usually doing something its plan() should have done instead.
        self.mode = mode
        # The Plan the last run of this command produced, when the runtime has
        # one, so `plan.diff(ctx.previous_plan)` shows what CHANGED rather than
        # listing everything again. None on a first run, which is the right
        # answer and not a special case.
        self.previous_plan = previous_plan
        # The preview band's scene, for a command that declares preview_kind=.
        # ALWAYS a real PreviewScene, never None, so a preview() function does not
        # have to branch on whether anyone is looking — building a fragment nobody
        # renders costs a few hundred triangles that are then dropped.
        # WHERE the user right-clicked, for an entry declared with
        # @evp.menu: "panel", "params", "param:<name>", "commands" or
        # "results". EMPTY for every other kind of invocation — a run, a
        # preview, an action-bar button — so `if ctx.region:` reads as "was I
        # invoked from the menu", and an entry declared for one region can still
        # tell WHICH control it was aimed at when it declared "params".
        self.region = region or ""
        self.scene = _preview.PreviewScene(preview_kind or "3d")
        self._lines = []
        self._log_name = log_name or _slug_name(name)

    @property
    def is_preview(self):
        return self.mode == "preview"

    @property
    def param(self):
        """The parameter name a "param:<name>" region names, or "".

        The common thing an entry wants out of `region`, spelled once here so
        every command does not re-split the string.
        """
        prefix = "param:"
        return self.region[len(prefix):] if self.region.startswith(prefix) else ""

    @property
    def log_path(self):
        return paths.log_path(self._log_name)

    def say(self, text=""):
        """Print a line to the palette console AND keep it for the log file.

        Both, always. The console is what the user watches while it runs; the
        log is what they can still read afterwards and paste into a message.
        """
        print(text)
        self._lines.append(text)
        return text

    def flush(self):
        """Write everything `say()` collected to the command's log.

        Call it in a `finally`: a probe whose log is only written on the happy
        path loses exactly the run you needed to read.
        """
        if not self._lines:
            return self.log_path
        path = paths.append_log(self._log_name, self._lines)
        self._lines = []
        print("log -> %s" % path)
        return path

    def show(self, body="", questions=(), echo=True):
        """Write the command's whole result panel, questions LAST.

        ⚠️ USES `evp.ui.text`, NOT `evp.ui.table`. The results table does not
        update cell text correctly after the first fill
        (CMD-ResultsTableColumns), confirmed in Archicad on 2026-08-19;
        MassingFeasibility had already moved to text for the same reason. And
        the panel is a PROPORTIONAL font, so do not pad columns — one
        self-describing line each.

        `questions` is what only the user can answer: which way an arrow points,
        whether a row disappeared. It goes at the BOTTOM because that is where a
        reader ends up, and because a probe's questions were previously only in
        the log file — read after the run, when the answering is already over.

        `ui.text` REPLACES the panel on every call, so this is one call with
        everything rather than one per section.
        """
        from . import ui

        sections = [str(body).rstrip()] if str(body).strip() else []
        if questions:
            numbered = "\n".join(
                "%d. %s" % (index, line)
                for index, line in enumerate(questions, start=1))
            sections.append("NOW LOOK / REPORT\n" + ("-" * 40) + "\n" + numbered)

        panel = "\n\n".join(sections)
        ui.text(panel)
        if echo:
            # The panel is transient; the log is what survives the session and
            # can be pasted into a message.
            for line in panel.splitlines():
                self.say(line)
        return panel

    def __repr__(self):
        return "<Context %r mode=%s>" % (self.name, self.mode)


def _slug_name(name):
    """"Place Slope Symbols" -> "place_slope_symbols".

    paths.log_path slugs too, but it does not lower-case or collapse spaces to
    underscores, and `Place Slope Symbols.log` beside `massing_feasibility.log`
    would make the logs directory read as two conventions.
    """
    return "_".join(str(name).lower().split())
