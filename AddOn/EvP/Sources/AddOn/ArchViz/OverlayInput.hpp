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
//
// ⚠️ ONLY THE CANVAS THE RUNNING OVERLAY COMPOSES INTO, AND ONLY WHILE ITS HUD IS ON
// SCREEN. A message to any other window -- a palette floating over the HUD, a dialog --
// is never touched, and a HUD that is not being drawn (the 3D camera not locked yet,
// the overlay hidden) takes nothing: the user cannot be expected to avoid what they
// cannot see. The 3D canvas is the nominated swap chain's window; the plan's is the plan
// runtime's canvas.
//
// ⚠️ THE HOOK DECIDES AND RETURNS. It runs inside Archicad's dispatch: a rectangle test,
// a latch, a counter. No ACAPI, no layout, no lock.
//
// ⚠️ INSTALLED WITH THE FIRST CANVAS, REMOVED WITH THE LAST, AND AT UNLOAD. A hook that
// outlives this DLL is Windows calling into freed code; a latch that outlives its
// session would take the next session's first moves (§8). Every `Detach` resets it.
//
// MAIN THREAD, every entry point.

#include "ArchViz/OverlayHitMap.hpp"

#include <cstdint>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace geomsrv {
namespace archviz {
namespace overlayinput {

enum class View : uint8_t { ThreeD = 0, Plan = 1 };
const char* ViewName (View view);

// Whether the HUD of a view is on screen now. Called from the hook: atomics only.
using ShownTest = bool (*) ();

// The canvas `view`'s overlay composes into, and how to tell its HUD is drawn. Again
// with a different canvas: that one instead. The hook is installed with the first.
bool Attach (View view, HWND canvas, ShownTest shown, std::string& error);
// That view's overlay stopped: its canvas, its regions and the latch go; the hook with
// the last view.
void Detach (View view);
// Every view and the hook: the unload, and a project closing.
void Shutdown ();

// Where the HUD is on that view, from its last layout.
void SetHitMap (View view, HitMap map);

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
};
Stats GetStats ();

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv

#endif
