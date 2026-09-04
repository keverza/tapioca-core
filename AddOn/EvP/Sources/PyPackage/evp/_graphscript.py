"""Runs one node-graph script node's body.

⚠️ THIS IS NOT A COMMAND, AND THE DIFFERENCE MATTERS. A command
(``evp.command``) is something a user launches: it has a dialog, it may write to
the model, and it lives for as long as it takes. A graph script node is a
*function of its inputs* that the evaluator may run many times a second while
somebody drags a slider. So this module gives it none of a command's machinery -
no ``run()`` contract, no parameters dialog, no result store - and instead gives
it the two things a graph node needs: its inputs as ordinary names, and its
outputs read back out.

⚠️ THE NAMESPACE IS FRESH EVERY CALL. A module cached in ``sys.modules`` would
let one node's globals leak into the next node's script, which reads as a node
that works until somebody reorders the graph - about the worst debugging
experience this feature could offer.

⚠️ A NODE IS A FOLDER, AND ITS HELPERS *ARE* CACHED MODULES - which is the one
exception to the paragraph above, and it is a narrow one. ``import
calculations`` next to ``main.py`` goes through the ordinary import machinery, so
it lands in ``sys.modules`` like anything else. Two rules keep that honest:
``_invalidate`` drops exactly the modules whose file lives under this node's own
roots before each run (so a saved helper is picked up, and a stale one never
runs), and it never touches anything else - numpy is imported once per session
and reloading it per node evaluation would be both wrong and ruinously slow.

⚠️ AND THE ROOTS COME OFF sys.path AGAIN. The graph evaluates many nodes; a
path entry left behind by node A is a module node B can import by accident, and
the failure surfaces as a node that works until the graph is opened in a
different order. Added at the FRONT so a node's own helper beats a same-named
package in site-packages, and removed in a ``finally``.

⚠️ AND THE BUDGET IS ENFORCED FROM INSIDE THE INTERPRETER. A script node runs
code written by whoever is sitting at the machine, so it is the one body in the
catalog that will actually contain ``while True:``. A watchdog thread has nothing
to interrupt - the script is not blocked, it is running - so the only place to
stop it is a trace function the interpreter itself calls.

The driver on the C++ side is EvPPy.cpp's ``EvpPy_RunGraphScript``; the value
shapes are fixed by NodeGraph/ScriptValueJson.hpp.
"""

from __future__ import annotations

import importlib
import io
import os
import sys
import time
from typing import Any

# Matches ScriptRuntimeJs's ceiling. A script logging in a loop must not become
# unbounded memory in Archicad's process before the budget stops it.
_MAX_LOG_LINES = 200


class _Budget:
    """Stops the script when its wall-clock budget is gone.

    Checked on every line rather than every call: a ``while True: pass`` never
    makes a call, so a call-only trace would never fire on the one shape of
    runaway that matters most.
    """

    def __init__(self, milliseconds: float) -> None:
        self._deadline = time.monotonic() + (milliseconds / 1000.0)
        # Checked every N lines. Calling monotonic() on every line of a tight
        # numeric loop is itself a meaningful slowdown, and the budget does not
        # need that resolution.
        self._countdown = 0

    def __call__(self, frame: Any, event: str, arg: Any) -> Any:
        if event == "line":
            self._countdown -= 1
            if self._countdown <= 0:
                self._countdown = 2000
                if time.monotonic() > self._deadline:
                    raise TimeoutError(
                        "the script ran longer than its time budget and was stopped"
                    )
        return self


class _Capture(io.TextIOBase):
    """Collects what the script printed.

    Not a nicety: a script node runs on a worker thread inside Archicad with no
    console attached, so without this ``print`` goes nowhere and the most common
    way anyone debugs a script silently does not work.
    """

    def __init__(self, lines: list[str]) -> None:
        self._lines = lines
        self._partial = ""

    def write(self, text: str) -> int:
        self._partial += text
        while "\n" in self._partial:
            line, self._partial = self._partial.split("\n", 1)
            self._append(line)
        return len(text)

    def flush(self) -> None:
        if self._partial:
            self._append(self._partial)
            self._partial = ""

    def _append(self, line: str) -> None:
        if len(self._lines) < _MAX_LOG_LINES:
            self._lines.append(line)
        elif len(self._lines) == _MAX_LOG_LINES:
            self._lines.append("... further output suppressed")


def _normalise(root: str) -> str:
    """A comparable form of a folder path.

    Case-folded because this is Windows and ``C:\\Tapioca`` and ``c:\\tapioca``
    are one folder; ``normpath`` because a trailing separator would make the same
    root look like two entries on ``sys.path``.
    """
    return os.path.normcase(os.path.normpath(root))


# Every folder that has EVER been a script node's import root in this session.
#
# ⚠️ INVALIDATION HAS TO LOOK WIDER THAN THE NODE BEING RUN, AND THIS IS WHY. Two
# nodes may each keep a `helpers.py`; the first to run caches `helpers` pointing
# at ITS folder, and the second - invalidating only its own root - would import
# the first node's module under its own name and run somebody else's code. The
# set is bounded by how many script folders a session touches, and holds paths
# rather than modules, so nothing is kept alive by it.
_SCRIPT_ROOTS: set[str] = set()


def _discard_bytecode(cached: str | None) -> None:
    """Remove one dropped module's ``__pycache__`` entry.

    ⚠️ WITHOUT THIS, A HELPER EDITED TWICE IN THE SAME SECOND GOES ON RUNNING THE
    OLD CODE. A ``.pyc`` records its source's modification time TRUNCATED TO WHOLE
    SECONDS, alongside the source's size; an import is served from bytecode when
    both still match. Change `return 1` to `return 2` and save within a second of
    the previous save - which is what fixing a typo looks like - and the two
    sources have the same length and the same recorded second, so Python
    reasonably concludes nothing changed. Dropping the module from ``sys.modules``
    does not help: the next import reads the same stale bytecode.

    Only ever called for a module this function is already discarding, so the
    file removed is one belonging to the user's own script folder. Best effort: a
    read-only cache or a file another process holds open is not worth failing a
    run over, and the worst case is the staleness this exists to prevent.
    """
    if not cached:
        return
    try:
        os.remove(cached)
    except OSError:
        pass


def _invalidate(roots: list[str]) -> None:
    """Drop cached modules that live under any known script root.

    ⚠️ THIS IS WHAT MAKES "SAVE THE HELPER, SEE THE CHANGE" WORK. Python caches a
    module the first time it is imported and never looks at the file again; a node
    whose ``calculations.py`` was just edited would go on running the previous
    version for the rest of the Archicad session, while the editor showed the new
    one - the single most confusing failure this feature could have.

    ⚠️ AND IT TOUCHES ONLY THE USER'S OWN FOLDERS. A module with no ``__file__``
    (a builtin), or one whose file is anywhere else (numpy, the ``evp`` package,
    the standard library), is left alone. Reloading those per node evaluation
    would be slow, and for anything holding state it would be wrong.
    """
    if not roots:
        return
    _SCRIPT_ROOTS.update(_normalise(root) for root in roots)
    prefixes = tuple(root + os.sep for root in _SCRIPT_ROOTS)
    doomed = []
    for name, module in sys.modules.items():
        origin = getattr(module, "__file__", None)
        if not origin:
            continue
        if _normalise(origin).startswith(prefixes):
            doomed.append((name, getattr(module, "__cached__", None)))
    for name, cached in doomed:
        sys.modules.pop(name, None)
        _discard_bytecode(cached)

    # The finder caches a directory's contents, so a helper CREATED since the
    # last import would not be found without this - which is exactly what "add a
    # file in the editor, import it, run" does.
    importlib.invalidate_caches()


def run(
    source: str,
    path: str,
    inputs: dict[str, Any],
    outputs: list[dict[str, str]],
    import_roots: list[str] | None = None,
    budget_ms: float = 1000.0,
) -> dict[str, Any]:
    """Execute ``source`` with ``inputs`` bound, and read ``outputs`` back.

    Never raises. Every failure - a syntax error, an exception, a timeout, an
    output the script forgot to set - comes back as ``ok: False`` with a message
    someone can act on, because the caller is a node body inside Archicad and an
    exception crossing the extern-C boundary is undefined behaviour.
    """
    log: list[str] = []
    result: dict[str, Any] = {"ok": False, "error": "", "log": log, "outputs": {}}

    namespace: dict[str, Any] = {"__name__": "__evp_script__", "__file__": path}
    namespace.update(inputs)

    capture = _Capture(log)
    saved_out, saved_err = sys.stdout, sys.stderr
    budget = _Budget(budget_ms)

    # De-duplicated, keeping the caller's order: the node's own folder is first
    # and must stay first, because that is what lets a node shadow a shared
    # helper on purpose. A root that appears twice would otherwise be removed
    # once and left behind once.
    roots: list[str] = []
    for root in import_roots or []:
        if root and root not in roots:
            roots.append(root)

    saved_path = list(sys.path)
    try:
        if roots:
            sys.path[:0] = roots
            _invalidate(roots)
        sys.stdout = capture
        sys.stderr = capture
        try:
            compiled = compile(source, path or "<script>", "exec")
        except SyntaxError as error:
            # The line is what the author needs; they have the same file open in
            # another window with the same numbering.
            result["error"] = f"line {error.lineno}: {error.msg}"
            return result

        sys.settrace(budget)
        try:
            exec(compiled, namespace)  # noqa: S102 - running the user's script IS the feature
        finally:
            sys.settrace(None)
    except TimeoutError as error:
        result["error"] = str(error)
        return result
    except BaseException as error:  # noqa: BLE001 - nothing may escape to the C boundary
        # ⚠️ BaseException, not Exception. A script that calls sys.exit() raises
        # SystemExit, and letting that through would take the interpreter - and
        # therefore Archicad - down over a stray line in somebody's node.
        line = ""
        traceback = getattr(error, "__traceback__", None)
        while traceback is not None:
            if traceback.tb_frame.f_code.co_filename == (path or "<script>"):
                line = f"line {traceback.tb_lineno}: "
            traceback = traceback.tb_next
        result["error"] = f"{line}{type(error).__name__}: {error}"
        return result
    finally:
        capture.flush()
        sys.stdout, sys.stderr = saved_out, saved_err
        # Restored wholesale rather than by removing what was added: the script
        # may have appended to sys.path itself, and leaving that behind would let
        # one node change what the next one can import.
        sys.path[:] = saved_path

    produced: dict[str, Any] = {}
    for port in outputs:
        name = port["portId"]
        if name not in namespace:
            # Named, always. "The script did not set 'area'" is something the
            # author fixes by looking at one line.
            result["error"] = f"the script did not set '{name}'"
            return result
        produced[name] = namespace[name]

    result["outputs"] = produced
    result["ok"] = True
    return result
