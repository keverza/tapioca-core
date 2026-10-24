// ArchViz/OverlayInputResidue -- what could still own or block the pointer after the overlay's
// input was torn down (OverlayInput.hpp `Residue`).
//
// Overlay contract: private/docs/architecture/diligent/OVERLAY-INVARIANTS.md. §8: every Stop
// leaves nothing for the next session; §7: a step that can decline says why, and a verdict
// prints what it measured. This reads only -- it installs, removes and repairs nothing.
//
// MAIN THREAD: `GetCapture` answers for the calling thread's input queue.
#include "ArchViz/OverlayInput.hpp"

#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayinput {

namespace {
void Add (std::string& out, bool residue, const std::string& what)
{
    if (!residue)
        return;
    if (!out.empty ())
        out += ", ";
    out += what;
}
} // namespace

std::string Residue ()
{
    const Stats s = GetStats ();
    std::string out;
    Add (out, s.installed, "message hook");
    Add (out, s.mouseHook, "mouse hook");
    Add (out, s.keyboardHook, "keyboard hook");
    Add (out, s.idleHook, "idle hook");
    Add (out, s.returnHook, "return hook");
    Add (out, s.inputWindow, "input window");
    Add (out, s.roofEditKeys, "roof edit keys latched");
    Add (out, s.hudTextKeys, "HUD text keys latched");
    Add (out, s.gestureHeld, "a gesture's canvas still owns the release");
    Add (out, s.buttonsDown != 0, "buttons held 0x" + std::to_string (s.buttonsDown));
    for (int i = 0; i < 2; ++i) {
        const std::string view = ViewName (View (i));
        Add (out, s.attached[i], view + " attached");
        Add (out, s.subclassed[i], view + " canvas subclassed");
        Add (out, s.locked[i], view + " locked");
    }
    // The thread's and the desktop's own mouse state: none of these is ours to set, but each is
    // a way the pointer can be dead to Archicad with every hook of ours gone.
    if (const HWND capture = ::GetCapture (); capture != nullptr) {
        wchar_t name[64] = {};
        ::GetClassNameW (capture, name, 64);
        char narrow[64] = {};
        ::WideCharToMultiByte (CP_UTF8, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr);
        Add (out, true, std::string ("Win32 capture on a ") + narrow + " window");
    }
    RECT clip = {};
    if (::GetClipCursor (&clip)) {
        const RECT all = { ::GetSystemMetrics (SM_XVIRTUALSCREEN), ::GetSystemMetrics (SM_YVIRTUALSCREEN),
                           ::GetSystemMetrics (SM_XVIRTUALSCREEN) + ::GetSystemMetrics (SM_CXVIRTUALSCREEN),
                           ::GetSystemMetrics (SM_YVIRTUALSCREEN) + ::GetSystemMetrics (SM_CYVIRTUALSCREEN) };
        Add (out, clip.left > all.left || clip.top > all.top || clip.right < all.right || clip.bottom < all.bottom,
             "cursor clipped");
    }
    CURSORINFO cursor = { sizeof cursor };
    if (::GetCursorInfo (&cursor))
        Add (out, (cursor.flags & CURSOR_SHOWING) == 0, "cursor hidden");
    if (const HWND front = ::GetForegroundWindow (); front != nullptr) {
        DWORD process = 0;
        ::GetWindowThreadProcessId (front, &process);
        Add (out, process == ::GetCurrentProcessId () && !::IsWindowEnabled (::GetAncestor (front, GA_ROOT)),
             "the front window is disabled");
    }
    return out.empty () ? "none" : out;
}

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv
