"""Plan Anchor Probe — Archicad's OWN wall outlines, drawn over the floor plan.

PUBLIC-PLAN-ANCHOR, the drawing half. The READ is settled (Wall Plan Outline Probe, 6 of
6, widths exact to 0.0 mm, mitres measured). This puts those outlines on screen
through the plan overlay's orthographic camera, which is itself confirmed
(public probe/RE63).

⚠️ THE ANCHORS ARE AN INSTRUMENT, NOT A DRAWING. Their entire job is to be
compared against lines Archicad already drew. THE QUESTION IS REGISTER, NOT
BEAUTY: does each red outline sit exactly on the wall it came from?

If both halves are right the anchors are invisible as a separate thing — they
trace Archicad's own wall edges. Anything else is the finding:

    a constant offset, correct shape    the camera and the geometry disagree
                                        about the origin
    correct at the centre, drifting
    towards the edges                   a scale error — the DPI class of bug
                                        public probe already fixed once
    rotated about the plan centre       the plan's view rotation was mismeasured
    only SOME walls anchored            the read skipped them; the count below
                                        says how many walls it actually found
    lines thickening as you zoom in     the screen-space expansion is not
                                        working and the width has become a world
                                        width

⚠️ A CURVED WALL IS A SEPARATE QUESTION, AND IT IS THE ONE UNSETTLED THING HERE.
The repo contradicts itself about Archicad's arc sign — CommandUtils says a
positive arcAngle bulges RIGHT, evp.elements.polygon_area treats it as bulging
LEFT — and no header settles it. So `arc_sign` is a knob: if a curved wall's
anchor bulges the WRONG WAY (a lens shape instead of tracing the wall), run this
again with arc_sign=-1 and say which one was right. Straight walls are
unaffected either way, so a project with no curved walls simply cannot answer it.

Evidence: logs\\plan_anchor.log, plus logs\\archviz.log for the layer's own lines.
"""

import time

import evp

_LOG_NAME = "plan_anchor"

_lines = []


def _say(text=""):
    print(text)
    _lines.append(text)


def _flush():
    print("log -> %s" % evp.paths.append_log(_LOG_NAME, _lines))


def _call(name, params=None):
    result = evp.api.call("Tapioca." + name, params or {}, raise_on_error=False)
    if not result.ok:
        _say("Tapioca.%s transport failure: %s" % (name, result.error))
        return None
    return result.data or {}


def _wait(seconds, message, hint):
    """Hold, keeping the instruction on the palette's STATUS LINE.

    ⚠️ NOT ONLY IN THE LOG. A user who reads what to look at after the run is
    over has produced a run in which "it did not work" and "nobody looked" are
    the same result.
    """
    deadline = time.time() + seconds
    while time.time() < deadline:
        remaining = int(deadline - time.time()) + 1
        evp.ui.progress("%s (%ds left)" % (message, remaining), hint=hint)
        time.sleep(0.5)


@evp.command(
    title="Plan Anchor Probe",
    category="Diagnostics",
    description="PUBLIC-PLAN-ANCHOR: draw Archicad's own wall plan outlines over the floor plan and check they register.",
    labels={
        "wait_seconds": "How long each look-at-it step waits",
        "width_pixels": "Anchor line width, in SCREEN pixels at every zoom",
        "arc_sign": "Which way a CURVED wall's arc bulges (+1 confirmed 2026-08-13)",
        "scope": "Anchor every wall on the storey, or only the selected ones",
        "attach": "How the overlay window is parented — see the log (public probe)",
        "close": "Just clear the anchors and take the overlay down",
    },
    runtime="embedded",
)
def run(wait_seconds: evp.Int(minimum=5, maximum=180) = 30,
        width_pixels: evp.Float(minimum=0.5, maximum=16.0) = 2.0,
        arc_sign: evp.Enum("+1", "-1") = "+1",
        scope: evp.Enum("all walls", "selected walls") = "all walls",
        attach: evp.Enum("child transparent", "child layered", "popup") = "child transparent",
        close: bool = False):
    try:
        # ⚠️ THE OFF SWITCH (public probe). The palette is never shown while an
        # overlay runs and the overlay is click-through, so without this input
        # there is no way to take it down short of closing Archicad.
        if close:
            _say("Clearing the anchors and closing the overlay on request.")
            evp.elements.set_plan_anchors([], enabled=False)
            _call("CloseDiligentOverlay")
            time.sleep(1.0)
            state = _call("DiligentViewportState") or {}
            _say("renderer running: %s" % bool(state.get("running")))
            return

        _say("=" * 70)
        _say("PLAN ANCHOR PROBE (PUBLIC-PLAN-ANCHOR)")
        _say("=" * 70)
        _say("")
        _say("OPEN QUESTIONS, numbered so the report can answer them by number:")
        _say("  1. do the anchor outlines appear over the floor plan at all?")
        _say("  2. do they REGISTER — each outline exactly on its own wall?")
        _say("  3. do they stay in register through a pan and a zoom?")
        _say("  4. does the line stay the SAME THICKNESS as you zoom?")
        _say("  5. curved walls, if the project has any: right way or inside out?")
        _say("  6. public probe: are Archicad's own CALLOUTS and FLOATING PANELS")
        _say("     visible ON TOP of the overlay, as they should be?")
        _say("")

        # ⚠️ THE SCOPE IS AN INPUT, NOT AN INFERENCE. The first version anchored
        # the selection whenever there was one, which meant a stray selection
        # silently produced "only some walls are outlined" — a reading that looks
        # exactly like the read having skipped them. Ask for what is wanted.
        walls = []
        if scope == "selected walls":
            walls = evp.selection.get()
            _say("Source: the CURRENT SELECTION (%d element(s))." % len(walls))
            _say("Non-walls are skipped, so a mixed selection is fine.")
            if not walls:
                _say("⚠️ NOTHING IS SELECTED, so there is nothing to anchor. Select")
                _say("   some walls, or run with scope='all walls'.")
        else:
            _say("Source: EVERY wall the PROJECT lists.")
            # ⚠️ NOT `evp.model.elements` — that enumerates the extracted 3D
            # model, which a storey filter or a hidden layer empties while the
            # plan still draws every wall, producing a false "no walls found"
            # (measured 2026-08-14). Every coordinate drawn comes from 2D data
            # anyway, so the project's element table is the source that agrees
            # with what is on screen.
            result = evp.api.call("API.GetElementsByType", {"elementType": "Wall"},
                                  raise_on_error=False)
            walls = [record.get("elementId", {}).get("guid")
                     for record in ((result.data or {}).get("elements", [])
                                    if result.ok else [])
                     if record.get("elementId", {}).get("guid")]

        if not walls:
            _say("")
            _say("NO WALLS FOUND. Open a plan with walls, or select some, and")
            _say("run this again.")
            return

        # ---- the overlay ---------------------------------------------------
        state = _call("DiligentViewportState") or {}
        if state.get("running") and not state.get("overlay"):
            _say("")
            _say("The viewport is running in the PALETTE, not as an overlay.")
            _say("Closing it — one render thread, one surface.")
            _call("CloseDiligentViewport")
            time.sleep(1.5)
            state = _call("DiligentViewportState") or {}

        if not state.get("running"):
            _say("")
            _say("BRING THE FLOOR PLAN TO THE FRONT — the overlay attaches to")
            _say("whatever window is frontmost, and the camera is measured from it.")
            evp.ui.progress("starting the plan overlay",
                            hint="BRING THE FLOOR PLAN WINDOW TO THE FRONT NOW")
            time.sleep(2.0)
            _attach_mode = {"popup": 0, "child layered": 1, "child transparent": 2}[attach]
            _say("attach mode: %s (%d)" % (attach, _attach_mode))
            _call("OpenDiligentOverlay", {"attach": _attach_mode})
            for _ in range(20):
                time.sleep(0.5)
                state = _call("DiligentViewportState") or {}
                if state.get("running"):
                    break

        if not state.get("running"):
            _say("")
            _say("THE OVERLAY DID NOT START. The reason is in logs\\archviz.log.")
            _say("⚠️ public probe: the viewport opens ONCE per Archicad session. If")
            _say("   one was opened earlier, restart Archicad and run this first.")
            return

        _say("")
        _say("Overlay running. camera source: %s" % (state.get("cameraSource") or ""))
        if not state.get("overlay"):
            _say("⚠️ but it reports overlay=false, so it is NOT on the plan.")

        # ---- the anchors ---------------------------------------------------
        sign = -1 if arc_sign == "-1" else 1
        result = evp.elements.set_plan_anchors(walls, enabled=True,
                                               width_pixels=width_pixels,
                                               arc_sign=sign)
        _say("")
        _say("anchors handed over: %d wall(s), %d ring(s), %d vertices, accepted %s"
             % (result["count"], result["rings"], result["vertices"], result["accepted"]))
        _say("  arc sign %+d, width %.1f px" % (sign, width_pixels))
        if result["count"] < len(walls):
            _say("  (%d of the %d guids were not walls, and were skipped)"
                 % (len(walls) - result["count"], len(walls)))

        time.sleep(1.0)
        state = _call("DiligentViewportState") or {}
        _say("")
        _say("what the renderer says it is holding:")
        _say("  layer ready     : %s" % bool(state.get("planAnchorLayerReady")))
        _say("  anchors enabled : %s" % bool(state.get("planAnchors")))
        _say("  vertices        : %s" % state.get("planAnchorVertices"))
        _say("  width (px)      : %s" % state.get("planAnchorWidthPixels"))

        # ⚠️ THREE FIELDS BECAUSE "I SEE NOTHING" HAS THREE CAUSES, and on screen
        # they are one symptom. Name whichever one this is, here, rather than
        # making the user infer it.
        if not state.get("planAnchorLayerReady"):
            _say("")
            _say("⚠️ THE ANCHOR LAYER NEVER STARTED. Its shaders or pipeline did")
            _say("   not come up; the reason is in logs\\archviz.log. Nothing")
            _say("   below can be judged.")
        elif not state.get("planAnchorVertices"):
            _say("")
            _say("⚠️ THE LAYER IS LIVE AND HOLDING NOTHING. The read returned no")
            _say("   geometry — check the wall count above, and run Wall Plan")
            _say("   Outline Probe, which reports the read on its own.")

        _say("")
        _say("=" * 70)
        _say("NOW LOOK — the part only you can see")
        _say("=" * 70)
        _say("(a) THE PLAN, WITHOUT TOUCHING ANYTHING. Red outlines should trace")
        _say("    your walls. Look at a CORNER first: the anchor should follow the")
        _say("    mitre, not run through it.")
        _wait(wait_seconds, "look at the anchors over the plan",
              "DO THE RED OUTLINES SIT EXACTLY ON YOUR WALLS? Look at a corner.")

        _say("")
        _say("(b) ZOOM IN, THEN OUT. Two different things to watch:")
        _say("    - do the outlines STAY on the walls?")
        _say("    - does the line stay the same THICKNESS? It should look the")
        _say("      same weight at every zoom. If it fattens as you zoom in, the")
        _say("      screen-space width is not working.")
        _wait(wait_seconds, "zoom in and out",
              "ZOOM IN AND OUT — do the outlines stay put, and stay the same "
              "line weight?")

        _say("")
        _say("(c) PAN THE PLAN. They should follow, and land back exactly.")
        _wait(wait_seconds, "pan the plan",
              "PAN THE PLAN — do the anchors follow and land back in register?")

        _say("")
        _say("=" * 70)
        _say("REPORT — please answer by number")
        _say("=" * 70)
        _say("  1. did red outlines appear over the plan?")
        _say("  2. REGISTER: exact / offset (which way, roughly how far) /")
        _say("     scaled (say roughly what %) / rotated / only some walls?")
        _say("  3. do they hold register through a zoom and a pan?")
        _say("  4. does the LINE WEIGHT stay constant as you zoom?")
        _say("  5. curved walls: does the anchor follow the wall's own curve, or")
        _say("     bulge the wrong way? (if the wrong way, run again with")
        _say("     arc_sign=-1 and say whether that fixed it)")
        _say("  6. THE Z-ORDER QUESTION (public probe), and it needs THREE answers:")
        _say("     - is the overlay VISIBLE at all?")
        _say("     - do Archicad's CALLOUTS/tooltips appear ON TOP of it?")
        _say("     - do FLOATING PANELS stay on top when they overlap the view?")
        _say("     If the overlay is invisible, run again with attach='child")
        _say("     layered', then attach='popup'. The three modes differ by how")
        _say("     the window is parented and by one style bit; exactly one")
        _say("     should be visible AND click-through AND below the callouts.")
        _say("  7. FPS badge, top-right: did Archicad itself stay responsive?")
        _say("")
        _say("The overlay is STILL OPEN. Run this probe again with close=true to")
        _say("take it down.")

    finally:
        # ⚠️ IN `finally`, ALWAYS. A probe that throws before flushing has
        # produced no evidence at all, which is worse than a failed run.
        _flush()
