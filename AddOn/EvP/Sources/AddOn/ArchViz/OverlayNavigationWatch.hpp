// ArchViz/OverlayNavigationWatch -- what the overlay did to Archicad's own navigation, counted.
//
// Overlay contract: private/docs/architecture/diligent/OVERLAY-INVARIANTS.md. §7: deltas, never
// totals, and nothing concluded from a counter that is zero. A diagnostic: it changes no route,
// no redraw and no layout; it is told what happened and says it once every ~5 s.
//
// ⚠️ WHY IT EXISTS (the user, 2026-11-05 13:13-13:16, virtual editor on, Shift+middle orbit):
// the overlay drew every frame with that frame's camera (age 0 on 5142 of 5142), yet the
// navigation stuttered, and sometimes the view went somewhere unexpected. Archicad's frame
// interval read p95 330-500 ms in some seconds while the overlay spent under 10 ms at Present,
// so the time went on the main thread between frames -- and the 3D side measured nothing there.
// The questions, each a counter:
//   - Was Archicad's last pointer move far from where a navigation began? A locked view eats
//     every hover move (`MouseProc`), so Archicad may hold a position from long ago.
//   - Were moves eaten after the button came up while the gesture was Archicad's? `MouseProc`
//     reads the live buttons when the move is RETRIEVED, which a stalled main thread delays.
//   - Did a 3D redraw land within 300 ms of a wheel step Archicad was zooming by? `Redraw3D`
//     holds redraws only while the middle button is held.
//   - How long did the heartbeat and the HUD's layout hold the main thread, with the middle
//     button held and without?
//
// MAIN THREAD only: the mouse hooks, the heartbeat and `Redraw3D` all run there.

#ifndef GEOMSRV_ARCHVIZ_OVERLAYNAVIGATIONWATCH_HPP
#define GEOMSRV_ARCHVIZ_OVERLAYNAVIGATIONWATCH_HPP

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace navwatch {

// A pointer move over a canvas the input layer serves, in screen pixels: passed to Archicad, or
// eaten -- and if eaten, whether the router still gave the gesture to Archicad when it was.
void NoteMove (long x, long y, bool passed, bool eatenFromHostGesture);

// A middle press passed to Archicad: a navigation begins at (x, y), on a locked view or not.
void NoteNavigationStart (long x, long y, bool locked);

// The middle release Archicad was given: its gesture ends.
void NoteNavigationEnd ();

// A wheel step passed to Archicad.
void NoteWheel ();

// A 3D redraw asked of Archicad (`ACAPI_View_Redraw`), outside a held navigation.
void NoteRedraw ();

// The HUD laid out for the pointer, in microseconds.
void NoteHudRefresh (uint32_t micros);

// A key the HUD took from Archicad (`QueueKey`), by virtual-key code, auto-repeat or not.
// ⚠️ (2026-11-05 13:46-13:48) Shift+middle orbits the view, and over the locked view the HUD
// takes every key -- Shift and its ~30 auto-repeats a second included -- each asking for a
// layout of ~9 ms: 94 layouts in 5 s while the middle button was held.
void NoteKeyTaken (uint32_t code, bool repeat);

// A middle-button double-click passed to Archicad, which fits the view to the window: one
// line each, with when and where, so a jump the user noted can be matched to it.
void NoteMiddleDoubleClick (long x, long y);

// One heartbeat of the overlay's timer, start to end; the report is written from its end.
class Heartbeat {
  public:
    Heartbeat ();
    ~Heartbeat ();
    Heartbeat (const Heartbeat&) = delete;
    Heartbeat& operator= (const Heartbeat&) = delete;

  private:
    uint64_t started_ = 0;
    bool navigating_ = false;
};

// Session state (§8): every count and mark forgotten.
void Reset ();

// ⚠️ THE JUMP IS READ FROM THE CAMERA (2026-11-05 14:35-14:37): the user's pauses placed each
// jump within 5-15 s, and no counter told a jumping gesture from a smooth one. Every decode of
// the selected camera (`freshness::TakeCameraSamples`) is a step from the one before; a step
// turning or moving at five times the recent median rate, and by more than 3 deg or 0.1, writes
// a NAVJUMP line -- when, how far, and the gesture it fell in -- and every gesture writes a
// NAVGESTURE line once its samples are in, so the two can be compared.
struct CameraStep {
    double degrees = 0.0; // the rotation between the two views
    double travel = 0.0;  // the eye's displacement, in model units
};
// Pure: two row-vector rigid views of 16 floats.
CameraStep Step (const float* a, const float* b);

} // namespace navwatch
} // namespace archviz
} // namespace geomsrv

#endif
