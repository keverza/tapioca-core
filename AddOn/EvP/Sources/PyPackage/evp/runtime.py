"""E9 — how a running command notices it has been told to stop.

A command runs on a worker thread the palette detaches. Three things can ask it to
stop: the palette's **Stop** button, **closing the EvP panel**, and the command's
own ``@evp.command(timeout_s=...)`` deadline. All three set one token in the
add-on; this module is how Python reads it.

**You usually do not need to call anything here.** Once a run is cancelled the bus
refuses every call, and ``evp.api.call`` raises :class:`evp.Cancelled` — so a
command that talks to Archicad at all unwinds through its own ``finally`` blocks
without a single line of cancellation code. The palette then reports "cancelled",
not "FAILED", and writes no traceback.

What this module is for is the case the bus cannot see: a **pure-compute loop**
that goes seconds or minutes without asking Archicad for anything. Nothing can
interrupt that but the loop itself, so put a checkpoint in it::

    for chunk in chunks:
        evp.runtime.check_cancel()      # raises evp.Cancelled if stopped
        heavy_math(chunk)

Cadence: often enough that the user does not wait (a few times a second is
plenty), not so often that it dominates. The poll is a no-gate-hop atomic read —
microseconds, not the ~3ms a main-thread round trip costs — so a checkpoint per
iteration of anything doing real work is free. It is NOT free inside the innermost
loop of a tight numeric kernel; hoist it out one level.

Never swallow :class:`evp.Cancelled`. Let it propagate: both runners treat it as a
clean stop, and catching it turns "the user pressed Stop" into "the command
ignored the user".
"""

from .api import Cancelled, call


def should_cancel():
    """True if the running command has been asked to stop. Never raises.

    For a loop that has cleanup of its own to do before unwinding::

        if evp.runtime.should_cancel():
            writer.close()
            return

    Prefer :func:`check_cancel` when there is nothing to clean up — an exception
    that unwinds by itself is harder to forget than a return the caller must honour.
    """
    # raise_on_error=False on purpose: this must be safe to call from anywhere,
    # including a finally block that is already unwinding. Losing the poll is
    # better than raising a second exception over the first — and a bus that
    # cannot answer is not evidence of a cancel, so the answer is False.
    result = call("EvP.PollCancel", raise_on_error=False)
    return bool((result.data or {}).get("cancelled")) if result.ok else False


def check_cancel():
    """Raise :class:`evp.Cancelled` if the running command has been asked to stop.

    The checkpoint to sprinkle through a long compute loop. A no-op — one atomic
    read, no main-thread hop — when nothing has been cancelled.
    """
    result = call("EvP.PollCancel", raise_on_error=False)
    if not result.ok:
        return
    state = result.data or {}
    if state.get("cancelled"):
        raise Cancelled(state.get("reason") or "cancelled")


def cancel_reason():
    """Why the run is stopping ("stopped by the user", ...), or "" if it is not.

    For a log line on the way out. Do not branch on the exact wording — it is a
    human-readable phrase from the add-on, not an API.
    """
    result = call("EvP.PollCancel", raise_on_error=False)
    return (result.data or {}).get("reason", "") if result.ok else ""
