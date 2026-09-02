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

⚠️ AND THE BUDGET IS ENFORCED FROM INSIDE THE INTERPRETER. A script node runs
code written by whoever is sitting at the machine, so it is the one body in the
catalog that will actually contain ``while True:``. A watchdog thread has nothing
to interrupt - the script is not blocked, it is running - so the only place to
stop it is a trace function the interpreter itself calls.

The driver on the C++ side is EvPPy.cpp's ``EvpPy_RunGraphScript``; the value
shapes are fixed by NodeGraph/ScriptValueJson.hpp.
"""

from __future__ import annotations

import io
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


def run(
    source: str,
    path: str,
    inputs: dict[str, Any],
    outputs: list[dict[str, str]],
    budget_ms: float,
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

    try:
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
