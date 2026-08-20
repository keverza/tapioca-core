"""Layer 2 — "has the model changed since I last looked" (E25).

    evp.changes.watch()                 # arm it once, main-thread, not cheap
    seen = evp.changes.token()          # the counter right now
    ...
    if evp.changes.token() != seen:     # cheap: no gate hop, poll at will
        refresh()

    # or, for a viewer that should redraw when the user stops dragging:
    seen = evp.changes.wait_for_change(seen, settle=0.3, timeout=None)

WHY IT IS A TOKEN AND NOT A CALLBACK. Archicad's element observer fires on
Archicad's own thread, in the middle of the user's edit — nothing useful can be
done there. So the add-on's observer does the least it can (bump a counter, note
the guid) and a command POLLS the counter from its own worker thread, where
blocking and rebuilding are allowed.

THREE SOURCES OF "WHAT CHANGED", and picking the wrong one is how this got slow:

| | cost | reports |
|---|---|---|
| `watch()` (default) | free | element CREATION only |
| `watch(guids=[...])` | one attach per guid | those elements, fully |
| `model_diff()` | ONE call, no observers | new / modified / deleted, whole model |
| `token()` / `poll()` | gate-free, inline | that anything happened |

⚠️ DO NOT ASK FOR AN OBSERVER ON EVERY ELEMENT. `watch(attach_all=True)` on a
12,238-element project measured ~9 attaches per second — ~22 minutes, poking
Archicad twice a second the whole way. `model_diff()` answers the same question
in one call. `attach_all` is opt-in by name for exactly this reason.

⚠️ WITHOUT `guids` OR `attach_all`, ONLY CREATION IS REPORTED. Changes and
deletions to elements that already existed are not, because Archicad sends those
only for elements with an observer attached. `poll()["watching"]` goes back to
False when the project changes.

⚠️ BRANCH ON `complete` BEFORE `guids`. An edit storm (select-all, move) overruns
the add-on's fixed ring, and a `since` window older than the ring returns
`complete: False`. That means "I no longer know which elements changed" — the
only correct reaction is a full refresh, never a partial update from a list that
looks short but isn't.

⚠️ THE TOKEN IS PER-SESSION, not per-project, and only ever goes up. Opening a
different document bumps it rather than resetting it, so a stale comparison
reads as "changed" and never as "unchanged".
"""

import time as _time

from .api import call
from . import runtime as _runtime


def watch(guids=None, attach_all=False, scope="3d", enable=True, background=None,
          slice_ms=None, gap_ms=None, max_elements=None, budget_ms=None):
    """Arm the change observer. Call once before you start polling.

    ⚠️ PER-ELEMENT OBSERVERS DO NOT SCALE, and the default reflects that.
    Measured on a real 12,238-element project: attaching to every element managed
    ~9 per second (each gate round trip waits ~190 ms to be dispatched to carry
    ~6 ms of work), so a full pass would take ~22 MINUTES of poking Archicad
    twice a second. Slicing cannot fix a total that large.

    So, cheapest first:

    **`scope="visible"`** — attach to what the CURRENT 3D VIEW shows, and nothing
    else. Hidden layers, other storeys and filtered-out elements cannot change a
    pixel, so watching them is pure cost. Bounded by the view rather than the
    database: the measured project had 12,238 elements, of which only the current
    sight matters. ⚠️ Needs a generated 3D view — open the 3D window once first.
    **This is the right call for a viewer.**

    **Default (no arguments).** Installs the handlers and attaches to NOTHING.
    Element CREATION is reported at zero cost, because
    ACAPI_Element_CatchNewElement is global and free. Changes and deletes are
    not reported — pick one of the next two for those.

    **`guids=[...]`** — attach to exactly the elements you display. Bounded by a
    list you chose, done inline. The right call for a viewer.

    **:func:`model_diff`** — Archicad's own difference generator answers "what
    changed" in ONE call with no observers at all. The scalable answer, and the
    one to reach for on a big project.

    **`attach_all=True`** — the whole-model pass, opt-in by name. Background by
    default (worker thread, ~6 ms slices, 25 ms gaps, HARD 60-second wall, then
    it gives up and says so); `background=False` for the synchronous version
    capped by `max_elements`/`budget_ms` with a Cancel button.

    Returns {watching, mode, token, ...}. `observed`, where present, is
    ARCHICAD's count — believe it over `attached`.

    Re-arming is expected after a project change (Archicad drops every observer
    with the old document) — do it on `watching: False`, not on a schedule.
    """
    params = {"enable": bool(enable), "scope": str(scope)}
    if guids:
        # ⚠️ The wire is {elements:[{elementId:{guid}}]}, not {guids:[…]}, and
        # WatchModel's input schema is closed — a flat array fails
        # SchemaValidationFailed before the handler runs. See evp.model._element_id.
        params["elements"] = [{"elementId": {"guid": str(guid)}} for guid in guids]
    if attach_all:
        params["attachAll"] = True
    if background is not None:
        params["background"] = bool(background)
    if slice_ms is not None:
        params["sliceMs"] = int(slice_ms)
    if gap_ms is not None:
        params["gapMs"] = int(gap_ms)
    if max_elements is not None:
        params["maxElements"] = int(max_elements)
    if budget_ms is not None:
        params["budgetMs"] = int(budget_ms)
    return call("EvP.WatchModel", params).data or {}


def arm_progress():
    """How far an `attach_all` pass has got: {running, done, gaveUp, listed,
    attached, failed, slices, listMs, longestHoldMs, longestRoundTripMs,
    elapsedMs, phase}. Gate-free, like everything on the poll path.

    ⚠️ TWO DIFFERENT TIMINGS, and conflating them made the first live run
    unreadable. `longestHoldMs` is time spent INSIDE a slice — how long Archicad
    was actually held, the number the "don't slow Archicad" requirement is about.
    `longestRoundTripMs` is the whole gate call seen from the worker, which is
    mostly WAITING TO BE DISPATCHED; it says how busy Archicad is, not how much
    we cost it. Run 5 reported 1076 ms as a "main-thread hold" when it was almost
    entirely dispatch latency.
    """
    return (poll() or {}).get("armProgress", {})


def sync():
    """Force a change: bump the token as if the model had changed.

    THE MANUAL FALLBACK. A viewer's Sync/Refresh button calls this and then does
    exactly what it does for any other change, so there is ONE refresh path
    rather than a background one and a button one that drift apart.

    Also the honest escape hatch for the cases the observer cannot cover: an
    element the background pass has not reached yet, or a change Archicad reports
    to nobody.
    """
    return call("EvP.SyncModel").data or {}


def unwatch():
    """Stop observing, and STOP ANY BACKGROUND ARMING PASS. {watching: False, token}.

    The token keeps its value — it is a session counter, not a subscription.

    ⚠️ Call this if an `attach_all` pass is running and you want it to stop. It
    used to be un-callable-off: the worker outlived the command that started it
    and kept hitting the gate roughly twice a second.
    """
    return call("EvP.WatchModel", {"enable": False}).data or {}


def model_diff(scope="3d", reset=False):
    """What changed since the last call — NO OBSERVERS, one call. The scalable path.

    Returns {baseline, new, modified, deleted, environmentChanged, *Count,
    elapsedMs}. The FIRST call (or `reset=True`) only establishes a baseline and
    reports nothing: `baseline: True` means "nothing to compare against", which
    is not the same as "nothing changed" — do not treat the empty lists as a
    quiet model.

    Each later call diffs the project against the previous call and re-baselines,
    so a poll loop naturally reads "what changed since I last asked".

    `scope` is "3d" (geometry as the modeler sees it — what a viewer wants) or
    "file" (every element edit, including ones with no 3D consequence).

    ⚠️ IT IS A POLL, NOT A NOTIFICATION — which is the right shape when latency
    is acceptable and interference is not. ⚠️ Check `elapsedMs` before choosing a
    cadence; this is one main-thread call and its cost on a big project is not
    yet measured. Do not put it in a 300 ms loop on trust.
    """
    return call("EvP.GetModelDiff",
                {"scope": str(scope), "reset": bool(reset)}).data or {}


def observed(guids=None):
    """Which elements Archicad says this add-on observes — ITS tally, not ours.

    Without `guids`, returns {count}. With them, also `observed`: a bool per guid,
    parallel to the input.

    The diagnostic for "the token did not move". A change that is not reported
    has two opposite causes — the element was never watched (re-arm, widen the
    scope) or it WAS watched and Archicad sent nothing anyway — and from Python
    they look identical. This tells them apart.
    """
    params = {"guids": list(guids)} if guids else None
    return call("EvP.GetObservedElements", params).data or {}


def token():
    """The change counter right now, as an int.

    Gate-free. Compare it against the last value you saw; any difference means
    at least one element was created, changed or deleted in between. It says
    nothing about WHICH — for that, keep the old value and pass it to `poll`.
    """
    return int((call("EvP.GetChangeToken").data or {}).get("token", 0))


def poll():
    """The cheap heartbeat: {token, watching, watchedCount, idleMs, dirtyCount,
    arming, armProgress}.

    Gate-free. `dirtyCount` is the queue depth — how many DISTINCT elements are
    waiting. Claim them with :func:`take`.
    """
    return call("EvP.GetChangeToken").data or {}


def take(max_items=500, peek=False):
    """THE QUEUE DRAIN. Claim up to `max_items` dirty elements and remove them.

    Returns {guids, events, count, remaining, overflowed, token, idleMs}, with
    `guids` and `events` parallel. Each element appears ONCE with the last thing
    that happened to it, however many times it was touched — a drag's thousand
    frames coalesce into one entry, because the store is a set and not a log.

    ⚠️ DRAINED ENTRIES ARE GONE. If the update might fail, pass `peek=True`,
    do the work, then drain — nothing outside can re-mark an element dirty.

    ⚠️ `overflowed` means more elements changed than the queue can hold and some
    were dropped: the list is real but incomplete, so refresh wholesale.
    `remaining > 0` is not a problem — it just means drain again.
    """
    return call("EvP.TakeChanges",
                {"max": int(max_items), "peek": bool(peek)}).data or {}


def updates(settle=0.4, interval=0.5, max_items=500, timeout=None):
    """Yield batches of dirty elements as they settle. THE PIPELINE, as a loop.

        for batch in evp.changes.updates():
            push_to_webui(batch["guids"])      # already coalesced and batched

    This is the shape the consumer asked for — *element changed -> mark dirty ->
    queue -> update, batched*. It does not fire per notification: it waits for
    the model to be QUIET for `settle` seconds, then drains everything that
    accumulated as ONE batch. A ten-second drag produces one batch, not a
    thousand, and the elements in it are distinct.

    Blocks between batches, so run it on the command's own thread. Honours the
    run's cancel like every other waiting loop here. `timeout` (seconds) ends the
    generator if nothing arrives; None waits indefinitely.
    """
    started = _time.monotonic()
    while True:
        _runtime.check_cancel()
        state = poll()
        pending = int(state.get("dirtyCount") or 0)
        idle_ms = int(state.get("idleMs") or -1)

        if pending and (settle <= 0 or (idle_ms >= 0 and idle_ms >= settle * 1000.0)):
            batch = take(max_items=max_items)
            if batch.get("count"):
                started = _time.monotonic()      # progress: reset the timeout
                yield batch
                continue

        if timeout is not None and (_time.monotonic() - started) > timeout:
            return
        _time.sleep(interval)


def wait_for_change(since, settle=0.3, timeout=None, interval=0.25):
    """Block until the model changes AND stops changing, then return the new token.

    `settle` (seconds) is the point of this over a bare token comparison: an edit
    in progress fires a notification per frame, and rebuilding on the first one
    means fighting the user's drag with a stale answer. Nothing is returned until
    the model has been quiet for `settle` seconds. Pass settle=0 to return on the
    first notification.

    Returns the new token, or None on `timeout` (seconds; None waits forever).
    Raises evp.Cancelled if the run is cancelled while waiting — this loop can
    block for human time, so it MUST honour the cancel like every other one.
    """
    started = _time.monotonic()
    since = int(since)
    while True:
        _runtime.check_cancel()
        state = poll()
        current = int(state.get("token", 0))
        if current != since:
            idle_ms = int(state.get("idleMs", -1))
            if settle <= 0 or (idle_ms >= 0 and idle_ms >= settle * 1000.0):
                return current
            # Changed but still moving: keep waiting on the SAME `since`, so the
            # notifications arriving meanwhile fold into one wake-up.
        if timeout is not None and (_time.monotonic() - started) > timeout:
            return None
        _time.sleep(interval)
