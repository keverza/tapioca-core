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

// A wheel step passed to Archicad.
void NoteWheel ();

// A 3D redraw asked of Archicad (`ACAPI_View_Redraw`), outside a held navigation.
void NoteRedraw ();

// The HUD laid out for the pointer, in microseconds.
void NoteHudRefresh (uint32_t micros);

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

} // namespace navwatch
} // namespace archviz
} // namespace geomsrv

#endif
