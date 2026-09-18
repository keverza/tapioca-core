#ifndef EVP_ARCHVIZ_OVERLAYREDRAWBUDGET_HPP
#define EVP_ARCHVIZ_OVERLAYREDRAWBUDGET_HPP

// ArchViz/OverlayRedrawBudget — who may ask Archicad to redraw, how often, and
// why it stops.
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md
// BEFORE EDITING THIS FILE. It decides when production reaches into Archicad and
// asks for a frame, which is the one thing on the overlay path that can turn a
// stall into a storm.
//
// ⚠️ IT IS A BUDGET AND NOT A TRIGGER, AND THAT IS THE WHOLE
// DESIGN. `ACAPI_View_Redraw` is cheap to call and expensive to call often: the
// heartbeat runs four times a second and Present re-raises its request on every
// suppressed frame, so anything tied to either is a redraw storm. The budget is
// spent per EXTENT -- a new window size is a new problem and gets a fresh two
// attempts; the same extent twice unresolved is a failure and says so.
//
// ⚠️ AND IT IS THE 3D WINDOW THIS RUNTIME SERVES, NOT WHICHEVER
// IS IN FRONT. `ACAPI_View_Redraw` acts on the CURRENT window, so asking while a
// Floor Plan is frontmost would redraw the plan -- a different renderer, with its
// own overlay, for a staleness that is not its. Invariant 12: a 3D state must
// never reach into the plan.
//
// MAIN THREAD ONLY. Every call here is ACAPI.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace redrawbudget {

// Is the window in front the SAME 3D session this runtime serves? The first 3D
// window seen while serving is the one it owns; anything else waits.
bool FrontWindowIsServedSession ();

// One heartbeat's worth of arbitration: spend a redraw if one is wanted, the
// served window is in front, and the budget for this extent is not exhausted.
//
// ⚠️ `modelFramesSeen` IS THE COLD-START CASE AND IT IS
// NOT THE SAME PROBLEM AS A STALE EXTENT. A resize leaves a camera measured for
// a window that no longer exists; a cold start has no camera at all, because
// Archicad only redraws its 3D window when something changes and a menu click
// changes nothing. `CHAIN frames=0 draws=0/0 ... BLOCKED AT NoModelFrames` is
// what that looks like, and it is what the Tapioca 3D Overlay menu item produced
// every time: armed, correct, and waiting for a frame that was never coming.
//
// ⚠️ PRODUCTION MUST NOT DEPEND ON THE USER DOING WHAT
// A DIAGNOSTIC INSTRUCTS (§9). The regression command works only because it
// prints "NAVIGATE the 3D window for up to 30 s" and a person obeys it. The menu
// item has no such luxury and should not need one.
void Consider (uint64_t modelFramesSeen);

// Cumulative, for the health record.
uint64_t Requests ();

// ⚠️ EVERYTHING `Start` NEEDS IS CLEARED BY `Stop` (§8). The
// served window especially: a budget that still names the PREVIOUS session's 3D
// window would refuse every redraw of the new one and look exactly like a camera
// that would not lock.
void Reset ();

} // namespace redrawbudget
} // namespace archviz
} // namespace geomsrv

#endif
