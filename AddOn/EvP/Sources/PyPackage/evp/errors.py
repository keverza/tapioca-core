"""evp.errors — the Python half of the API error trail.

The C++ side (Sources/AddOn/Diagnostics/ApiError) decodes GSErrCodes and writes
what the ADD-ON saw. This module writes what the COMMAND saw, into the same
``logs/api_errors.log``, so one file reads in order across both languages:

    ...  ACAPI   EvP.CreateRoof       <- the ACAPI call and its decoded code
    ...  ENVELOPE [c142] EvP.CreateRoof  <- what the bus handed back
    ...  PYTHON  command.py:88 run()  <- the line that asked for it, and why

Three things live here that the add-on structurally cannot know:

  1. WHICH LINE ASKED. The add-on receives a command name; it has no idea a
     script called it from ``run()`` line 88 inside a loop over 40 roofs. That
     is usually the first thing you actually want.
  2. WHAT LED UP TO IT. Every ``evp.api.call`` is recorded in a small ring, so
     a failure is reported together with the calls before it. Reconstructing
     that sequence by hand from a half-remembered run is what burns sessions.
  3. PURE-PYTHON FAILURES. A TypeError in a command never reaches the bus at
     all, so nothing in C++ can log it.

NOTHING HERE IS OPT-IN. ``evp.api.call`` records and reports on its own. A
diagnostic you have to remember to switch on is not available for the run that
needed it — that run already happened. (``evp.api.debug(True)`` is the opposite
trade: it traces EVERY call including successes, which is too loud to leave on.)

Reading it back inside a command:

    except evp.EvpError:
        for line in evp.errors.trail():        # this process's recent calls
            log(line)
        for line in evp.errors.native_trail(): # the add-on's failure ring
            log(line)
        raise
"""

import collections
import datetime
import os
import sys
import traceback

from . import paths

# One log, both languages. The C++ side builds this same path from
# %LOCALAPPDATA%; the shared name is the whole point, so change it in both or
# neither (Sources/AddOn/Diagnostics/ApiError.cpp -> ApiErrorLogPath).
LOG_NAME = "api_errors"

# How many recent bus calls to keep. Small on purpose: this is "what led up to
# it", not a transcript — the full history is api_trace.log with debug(True).
TRAIL_CAP = 24

# Params echo cap, matched to the C++ side's kParamsEcho. Long enough to
# identify the call, short enough that one entry stays readable.
_PARAMS_ECHO = 400

# Commands that READ the trail are not part of it. A diagnostic that appears in
# its own output crowds out the calls you were trying to see: reading the trail
# around each of six provocations pushed 10 GetErrorTrail lines into a 14-line
# window and evicted most of the real work. Same reasoning as excluding
# Cancelled — the trail should hold signal, not its own observation.
_NOT_RECORDED = frozenset(["EvP.GetErrorTrail", "EvP.SetTracing"])

_trail = collections.deque(maxlen=TRAIL_CAP)


def log_path():
    """``logs/api_errors.log`` — the same file the add-on writes to."""
    return paths.log_path(LOG_NAME)


def _clip(text, limit=_PARAMS_ECHO):
    text = str(text)
    return text if len(text) <= limit else text[:limit] + "... [clipped]"


def _now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _caller_site(skip_modules=("evp",)):
    """The innermost frame OUTSIDE the evp package — i.e. the command's own line.

    Walking out of our own package is what makes this useful: the raw innermost
    frame is always somewhere in api.py, which nobody needs to be told. If a
    command calls through a helper in its folder, that helper's line is the
    answer, which is also right — it is the line a human would go edit.
    """
    package_dir = os.path.dirname(os.path.abspath(__file__))
    frame = sys._getframe(1)
    while frame is not None:
        filename = frame.f_code.co_filename
        if os.path.dirname(os.path.abspath(filename)) != package_dir:
            return "%s:%d in %s()" % (
                os.path.basename(filename), frame.f_lineno, frame.f_code.co_name)
        frame = frame.f_back
    return "(inside evp)"


def _append(block):
    """Append one block. Best-effort: a logging failure must never be the reason
    a command dies, and must never mask the error it was trying to record."""
    try:
        with open(log_path(), "a", encoding="utf-8") as handle:
            handle.write(block.rstrip("\n") + "\n")
    except OSError:
        pass


def record(command, params, ok, meta):
    """Note one completed bus call. Called by ``evp.api.call`` on every call,
    success or not — the successes are the context that makes the failure
    legible ("it did 39 roofs, then this one")."""
    if command in _NOT_RECORDED:
        return
    _trail.append("%s %-7s %-28s %6.1fms %s" % (
        "ok  " if ok else "FAIL",
        (meta or {}).get("call_id", "?"),
        command,
        (meta or {}).get("duration_ms", 0.0),
        _clip(params, 120) if params else ""))


def trail(limit=TRAIL_CAP):
    """The last bus calls THIS process made, oldest first, one line each."""
    entries = list(_trail)
    return entries[-limit:] if limit and limit < len(entries) else entries


def native_trail(limit=10):
    """The ADD-ON's failure ring (``EvP.GetErrorTrail``) — failures reported by
    native commands, including ones from earlier runs in this Archicad session.

    Never raises: this is called from except-blocks, where a second exception
    would replace the one you are trying to report.
    """
    try:
        from . import api
        result = api.call("EvP.GetErrorTrail", {"limit": limit}, raise_on_error=False)
        if not result.ok:
            return []
        return list((result.data or {}).get("entries") or [])
    except Exception as exc:                     # noqa: BLE001 — see docstring
        return ["(could not read the add-on's error trail: %s)" % (exc,)]


def report_call_failure(command, params, error, meta):
    """Write the PYTHON view of a failed bus call: the line that made it, and
    the calls before it. The add-on has already written its own view of the same
    failure; these sit next to each other and answer different questions."""
    error = error or {}
    lines = [
        "---------------------------------------------------------------",
        "%s  PYTHON  %s" % (_now(), command),
        "  from    %s" % _caller_site(),
        "  code    %s" % error.get("code", "?"),
        "  message %s" % error.get("message", ""),
    ]
    if error.get("detail"):
        lines.append("  detail  %s" % error["detail"])
    if params:
        lines.append("  params  %s" % _clip(params))
    if meta:
        lines.append("  meta    call_id=%s backend=%s zone=%s duration_ms=%.1f" % (
            meta.get("call_id", "?"), meta.get("backend", "?"),
            meta.get("zone", "?"), meta.get("duration_ms", 0.0)))

    # The preceding calls, not counting the one that just failed.
    previous = trail()[:-1]
    if previous:
        lines.append("  leading up to it:")
        lines.extend("    " + entry for entry in previous[-8:])

    _append("\n".join(lines))


def report_exception(exc=None, where=""):
    """Write an uncaught exception WITH the bus trail. Use in a command's
    top-level except when you want the traceback and the call sequence in one
    place; the traceback alone rarely says which Archicad call preceded it."""
    lines = [
        "---------------------------------------------------------------",
        "%s  PYTHON EXCEPTION%s" % (_now(), (" — " + where) if where else ""),
    ]
    text = traceback.format_exc() if exc is None else "".join(
        traceback.format_exception(type(exc), exc, exc.__traceback__))
    lines.extend("  " + line for line in text.rstrip("\n").split("\n"))

    entries = trail()
    if entries:
        lines.append("  bus calls leading up to it:")
        lines.extend("    " + entry for entry in entries[-12:])

    _append("\n".join(lines))
    return text


def summary(limit=6):
    """A short, human-readable block for a command's OWN log or an alert:
    where to look, plus the last few failures from both sides."""
    lines = ["error trail (full detail: %s)" % log_path()]
    for entry in trail(limit):
        lines.append("  py     " + entry)
    for entry in native_trail(limit):
        lines.append("  addon  " + entry)
    return "\n".join(lines)
