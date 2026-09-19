// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// ArchViz/Dxgi/CameraFreshness -- how old is the camera this Present draws with,
// and does it still belong to the window on screen? TWO DIFFERENT FAULTS.
//
// ⚠️ ORDINARY LATENESS IS NOT STALENESS AND MUST NOT BE
// TREATED AS IT. During navigation the selected draw sometimes has not happened
// yet in the frame being presented, so the overlay composes with the previous
// frame's camera and slips by one frame against the building. That is measured
// here and DELIBERATELY NOT SUPPRESSED: blanking those frames would trade a
// sub-pixel slip for visible flicker. `GhostMesh` binds no camera of its own and
// is displaced by exactly the same amount -- it simply has nothing behind it to
// line up against, which is why it looked healthier.
//
// ⚠️ A RESIZE IS THE OTHER FAULT AND HAS NO CLOCK. Scaling a
// window changes the swap chain WITHOUT Archicad redrawing its model, so no new
// camera is ever produced and the old projection persists until the user happens
// to orbit. A run read `vp=2176x1350 target=1221x987 LATE+15`: fifteen
// consecutive Presents drawing the building beside itself. That IS suppressed --
// one blank frame beats the building drawn twice -- and it raises a request for
// the redraw that can end it.
//
// THREAD SAFETY. `NoteTargetExtent`, `NoteAccepted`, `Stale`, `NoteSuppressed`
// and `NoteAge` run on Archicad's render thread inside Present: atomics only, no
// lock, no allocation, no ACAPI. `TakeRedrawRequest`, `Snapshot` and `Reset` are
// for the main thread.

#ifndef GEOMSRV_ARCHVIZ_DXGI_CAMERAFRESHNESS_HPP
#define GEOMSRV_ARCHVIZ_DXGI_CAMERAFRESHNESS_HPP

#include <cstdint>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace freshness {

// RENDER THREAD, first thing in Present. One DXGI call, no buffer fetched.
void NoteTargetExtent (IDXGISwapChain* swapChain);

// RENDER THREAD, when a camera snapshot is accepted: remember the window size it
// was measured for.
void NoteAccepted ();

// ANY THREAD, PURE. True when the accepted camera belongs to a window size that
// no longer exists.
//
// ⚠️ THE TWO EXTENTS ARE THE SAME QUANTITY MEASURED AT TWO
// TIMES, so this needs no tolerance -- unlike comparing the 3D view's viewport
// against the back buffer, which is legitimately a pixel narrower.
bool Stale ();

// RENDER THREAD, when a Present was refused because `Stale`. Counts it and
// raises the coalesced redraw request: only Archicad can end this, and only by
// redrawing its model.
void NoteSuppressed ();

// RENDER THREAD, once per Present that had a camera to judge.
void NoteAge (uint64_t presentGeneration, uint64_t snapshotGeneration);

// ANY THREAD. The window extent the composition is currently trying to reach,
// packed width << 16 | height, or 0 before the first Present.
//
// ⚠️ THIS IS THE RETRY EPOCH AND MODEL FRAMES ARE NOT.
// Bounding retries by "model generations advanced" is wrong: a redraw that
// advances the generation WITHOUT producing a camera accepted for this extent
// would reset the budget and buy another attempt, and a window that keeps
// redrawing for unrelated reasons would buy attempts forever. The attempt budget
// belongs to the extent being repaired, so two failures for THIS extent are two
// failures full stop.
uint32_t TargetEpoch ();

// ANY THREAD. The window extent the composition is currently trying to reach,
// packed width << 16 | height, or 0 before the first Present.
//
// ⚠️ THIS IS THE RETRY EPOCH AND MODEL FRAMES ARE NOT.
// Bounding retries by "model generations advanced" is wrong: a redraw that
// advances the generation WITHOUT producing a camera accepted for this extent
// would reset the budget and buy another attempt, and a window that keeps
// redrawing for unrelated reasons would buy attempts forever. The attempt budget
// belongs to the extent being repaired, so two failures for THIS extent are two
// failures full stop.

// MAIN THREAD. True once per raised request -- COALESCED, so a hundred suppressed
// Presents produce one. The caller decides whether it is allowed to act on it.
//
// ⚠️ HOW LONG IT WAITED IS MEASURED, BECAUSE ONLY ONE
// THING CONSUMES IT AND THAT THING IS A `WM_TIMER`. `OverlayRedrawBudget::
// Consider` is the sole caller, it runs from the injected runtime's 250 ms
// heartbeat, and `CameraWake.hpp` records what a `WM_TIMER` does under a drag:
// "synthesised only when the queue has nothing else in it ... during a drag
// Archicad's queue is never empty, so the timer is served last however short its
// interval", measured at 24-41 ms for a 15 ms request in 2026-08-13. A request
// raised by a SUPPRESSED Present -- the overlay is off the screen right now --
// waiting on the lowest-priority message Windows has is the shape of
// OVERLAY-INVARIANTS.md section 9, and the wait is the number that says so.
bool TakeRedrawRequest ();

struct Report {
    // ⚠️ BUCKETS, NOT A MEAN. A mean lets one forty-frame
    // stall hide a thousand good frames, and the question is what the STEADY
    // STATE does. `age0` is the overlay exactly on the building.
    uint64_t age0 = 0;
    uint64_t age1 = 0;
    uint64_t age2 = 0;
    uint64_t age3plus = 0;
    uint32_t ageMax = 0;
    uint64_t samples = 0;
    uint64_t suppressed = 0;

    // ⚠️ THE GAP BETWEEN THE OVERLAY GOING BLANK AND
    // ANYONE ASKING FOR THE FRAME THAT ENDS IT. `NoteSuppressed` raises the
    // request on the render thread the moment a Present draws nothing;
    // `TakeRedrawRequest` is reached only from the main-thread heartbeat. This is
    // the milliseconds between the two, worst case, and `redrawsTaken` is how
    // many requests ever got that far -- a request raised and never taken is an
    // overlay that stayed blank.
    uint32_t redrawWaitMaxMs = 0;
    uint64_t redrawsTaken = 0;
};
Report Snapshot ();

// MAIN THREAD, at the start of an overlay session.
void Reset ();

} // namespace freshness
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
