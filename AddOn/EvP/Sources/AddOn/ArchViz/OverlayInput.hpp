#ifndef EVP_ARCHVIZ_OVERLAYINPUT_HPP
#define EVP_ARCHVIZ_OVERLAYINPUT_HPP

// ArchViz/OverlayInput -- the mouse over the overlays' HUD. The HUD is drawn inside
// Archicad's own frame, at its Present (OVERLAY-INVARIANTS.md §2); it has no window of
// its own to receive input. So Archicad's mouse messages to the canvas the overlay
// composes into are looked at before Archicad dispatches them, and those that are the
// HUD's (OverlayHitMap.hpp says which) never reach Archicad.
//
// ⚠️ A THREAD-LOCAL `WH_GETMESSAGE` HOOK, AS CameraWake's. It sees every posted message
// of the main thread before its window procedure does, and discards one by making it
// `WM_NULL`. Never global and never low-level: those reach every process on the desktop.
// ⚠️ AND A THREAD-LOCAL `WH_MOUSE` HOOK FOR THE BUTTONS. Before GetMessage returns a button
// message Windows SENDS the canvas's parent WM_PARENTNOTIFY (and the canvas WM_MOUSEACTIVATE
// and WM_SETCURSOR): a rewrite at GetMessage is too late for those, and a right click on the
// HUD still reached Archicad (the user, 2026-09-30). The mouse hook runs before all of them
// and eats the HUD's presses and releases outright.
//
// ⚠️ ONLY THE CANVAS THE RUNNING OVERLAY COMPOSES INTO, AND ONLY WHILE ITS HUD IS ON
// SCREEN. A message to any other window -- a palette floating over the HUD, a dialog --
// is never touched, and a HUD that is not being drawn (the 3D camera not locked yet,
// the overlay hidden) takes nothing: the user cannot be expected to avoid what they
// cannot see. The 3D canvas is the nominated swap chain's window; the plan's is the plan
// runtime's canvas.
//
// ⚠️ THE HOOK DECIDES AND RETURNS. It runs inside Archicad's dispatch: a rectangle test,
// a latch, the pointer noted, a counter. No ACAPI, no layout, no lock. When the HUD has
// something to show for it -- the pointer over it or just off it, a press or release it
// took -- it posts ONE coalesced message to a window of its own, and the view's HUD is
// laid out again there, on the main thread at ordinary priority (CameraWake's shape).
//
// ⚠️ A STILL VIEW PRESENTS NOTHING. The plan presents 22-30 times a second from the
// cursor's feedback alone (HANDOFF-OverlayPatch 2026-09-28 09:41) -- but not while the
// HUD takes the moves. So a layout that changed what the HUD draws asks for the view's
// redraw, at most about thirty a second: the last change in a burst is always drawn,
// the ones between may not be. A layout that changed nothing asks for nothing, and
// nothing is laid out while Archicad owns the gesture (a wall drawn across a panel).
//
// ⚠️ THE HUD'S OWN CURSOR, FROM A SUBCLASS OF THE CANVAS. Windows asks the window under
// the pointer for its cursor with a SENT `WM_SETCURSOR`, which no message hook sees. So
// the canvas is subclassed (comctl32's `SetWindowSubclass`, chainable and removable) and
// answers it while the pointer is the HUD's: an arrow, or a hand over what the last
// layout found the pointer could press. Everything else goes on to Archicad. A canvas of
// another thread is neither subclassed nor seen by the hook; `Stats` says which.
//
// ⚠️ HOW LONG A CLICK KEEPS THE THREAD BUSY, ON DEMAND (OverlayClickTiming.hpp). Armed,
// a second thread-local hook, WH_FOREGROUNDIDLE, says when the main thread goes idle, and
// the message hook says when it wakes and what was pressed -- the HUD, the view, or any
// other window of the thread, a DG palette's among them. It measures only while the
// message hook is installed: while a view's HUD is attached.
//
// ⚠️ INSTALLED WITH THE FIRST CANVAS, REMOVED WITH THE LAST, AND AT UNLOAD. A hook or a
// window that outlives this DLL is Windows calling into freed code; a latch that
// outlives its session would take the next session's first moves (§8). Every `Detach`
// resets it, and takes the subclass off its canvas.
//
// MAIN THREAD, every entry point.

#include "ArchViz/OverlayClickTiming.hpp"
#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayHud.hpp"

#include <cstdint>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace geomsrv {
namespace archviz {
namespace overlayinput {

const char* ViewName (View view);

// What the owner of a view's overlay gives it.
struct HudOwner {
    // Whether the HUD is on screen now. Called from the hook: plain reads and atomics.
    bool (*shown) () = nullptr;
    // Lay the HUD out again for `TakeInput`; true when what it draws changed. From the
    // posted message, outside the hook: ACAPI allowed.
    bool (*refresh) () = nullptr;
    // Have Archicad draw the view again: the HUD changed and a still view presents
    // nothing. From the posted message or its timer: ACAPI allowed.
    void (*redraw) () = nullptr;
};

// The canvas `view`'s overlay composes into, and its owner. Again with a different
// canvas: that one instead. The hook is installed with the first. A canvas newly taken
// asks the owner for a layout (`refresh`): where the HUD is on it is known only then.
bool Attach (View view, HWND canvas, const HudOwner& owner, std::string& error);
// That view's overlay stopped: its canvas, its regions, its pointer and the latch go;
// the hook and the window with the last view.
void Detach (View view);
// Every view, the hook and the window: the unload, and a project closing.
void Shutdown ();

// Where the HUD is on that view, from its last layout.
void SetHitMap (View view, HitMap map);

// Lay that view's HUD out again soon, as a pointer change would: something other than the
// pointer changed what it shows (the text size set from Python). Nothing for a view not
// attached.
void RequestLayout (View view);

// The view's size and the pointer over it, for the HUD's layout. `TakeInput` hands over
// the presses and releases the HUD took since the last call; `CurrentInput` leaves them.
overlayhud::Input TakeInput (View view);
overlayhud::Input CurrentInput (View view);

// The click meter armed (and its samples forgotten) or disarmed; whether it is; what it
// holds, oldest first, and how often the thread went idle while it was armed.
void TimeClicks (bool on);
bool TimingClicks ();
std::vector<overlayclicks::Sample> ClickSamples ();
uint64_t ClickIdles ();

// ⚠️ TOTALS SINCE THE PROCESS STARTED: a question about now is two readings and their
// difference (§7). `declinedHidden` counts messages over a region while its HUD was not
// on screen -- passed, and counted so an overlay that never takes anything can say why.
struct Stats {
    bool installed = false;
    bool attached[2] = { false, false };
    uint32_t regions[2] = { 0, 0 };
    uint64_t seen = 0; // mouse messages to an attached canvas, removed from the queue
    uint64_t taken = 0;
    uint64_t takenPresses = 0;
    uint64_t takenMoves = 0;
    uint64_t passedOverHud = 0; // over a region but Archicad's: a gesture begun in the view, or navigation
    uint64_t declinedHidden = 0;
    uint64_t refreshes = 0; // the HUD laid out again for the pointer
    uint64_t changes = 0;   // ...and what it draws changed
    uint64_t redraws = 0;   // views asked to draw again for it
    uint32_t lastRedrawMicroseconds = 0;
    uint32_t maxRedrawMicroseconds = 0;
    uint32_t lastRefreshMicroseconds = 0;
    uint32_t maxRefreshMicroseconds = 0;
    // Whether each view's canvas is this thread's -- the hook sees nothing of one that is
    // not -- and whether its WM_SETCURSOR is answered; the cursors the HUD set, and how
    // many of them were the hand.
    bool canvasOnThread[2] = { false, false };
    bool subclassed[2] = { false, false };
    uint64_t cursorsSet = 0;
    uint64_t handsShown = 0;
    // The buttons: whether the mouse hook is in, the presses and releases it ate -- nothing
    // of Archicad's heard those -- and the ones only the message hook saw (posted, not from
    // the mouse; or no mouse hook). And the context menus asked for over the HUD, swallowed.
    bool mouseHook = false;
    uint64_t buttonsEaten = 0;
    uint64_t buttonsLate = 0;
    uint64_t contextMenusSwallowed = 0;
};
Stats GetStats ();

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv

#endif
