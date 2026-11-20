#ifndef TAPIOCA_OVERLAY_INPUT_DESKTOP_HPP
#define TAPIOCA_OVERLAY_INPUT_DESKTOP_HPP

// ⚠️ BOUND BY OVERLAY-INVARIANTS.md (§15, §16): what the input layer reads of the live desktop.
//
// The pointer's place, the window under it, the shown cursor, the held keys and buttons and the
// foreground window are not messages: they are the desktop's state at the moment of asking, and
// every route the cursor and the host mode take depends on them. In Archicad they are user32's and
// nothing else -- `live` is never replaced there.
//
// The offline tests replace it with a quiet desktop (nobody at the desk: the pointer elsewhere, the
// cursor showing, nothing held, no foreground window of theirs). Read live, a test run on a machine
// in use failed when its user typed (Windows hides the cursor, which reads as Archicad's Explore),
// held a button or left the pointer over a test window (the user, 2026-11-20).
//
// Archicad's own reads, answered by the pointer shim, and the residue report stay on user32: they
// are about the real desktop by definition.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace geomsrv::archviz::overlayinput {

struct Desktop {
    BOOL (WINAPI* cursorPos) (LPPOINT) = &::GetCursorPos;
    HWND (WINAPI* windowFromPoint) (POINT) = &::WindowFromPoint;
    BOOL (WINAPI* cursorInfo) (PCURSORINFO) = &::GetCursorInfo;
    SHORT (WINAPI* asyncKeyState) (int) = &::GetAsyncKeyState;
    SHORT (WINAPI* keyState) (int) = &::GetKeyState;
    HWND (WINAPI* foregroundWindow) () = &::GetForegroundWindow;
};

// The one the input layer reads. Tests only: set it with `desktop = {...}`, back with `desktop = {}`.
inline Desktop desktop;

} // namespace geomsrv::archviz::overlayinput
#endif
