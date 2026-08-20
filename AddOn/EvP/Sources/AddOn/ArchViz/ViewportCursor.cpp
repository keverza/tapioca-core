#include "ArchViz/ViewportCursor.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <windows.h>

namespace geomsrv {
namespace archviz {
namespace viewportcursor {

namespace {

HWND     gWindow = nullptr;
WNDPROC  gPreviousProc = nullptr;

LRESULT CALLBACK ViewportProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // ⚠️ ONLY WHEN THE POINTER IS OVER THE CLIENT AREA. `WM_SETCURSOR`'s low word
    // of lParam is the hit-test result, and it arrives for the frame and
    // scrollbars too. Claiming those would fight whatever DG wants there; the
    // client area is the part that is ours.
    if (message == WM_SETCURSOR && LOWORD (lParam) == HTCLIENT) {
        ::SetCursor (::LoadCursor (nullptr, IDC_ARROW));
        // ⚠️ TRUE MEANS "HANDLED, STOP". Falling through to the previous proc
        // would let DefWindowProc ask the PARENT, and the parent is the palette
        // -- which is where the splitter's resize arrow comes from in the first
        // place, so the default would immediately undo the line above.
        return TRUE;
    }

    if (gPreviousProc != nullptr)
        return ::CallWindowProc (gPreviousProc, hwnd, message, wParam, lParam);
    return ::DefWindowProc (hwnd, message, wParam, lParam);
}

}   // namespace

bool Attach (void* hwnd)
{
    HWND const window = (HWND) hwnd;
    if (window == nullptr || ::IsWindow (window) == FALSE)
        return false;

    if (gWindow == window && gPreviousProc != nullptr)
        return true;   // already ours

    // A different window than last time: put the old one back first, so a
    // reopened viewport cannot leave a subclass pointing at a dead HWND.
    if (gWindow != nullptr)
        Detach ();

    ::SetLastError (0);
    LONG_PTR const previous =
        ::SetWindowLongPtr (window, GWLP_WNDPROC, (LONG_PTR) &ViewportProc);
    if (previous == 0 && ::GetLastError () != 0) {
        ArchVizLog ("viewport cursor: SetWindowLongPtr failed; the pointer will keep "
                    "whatever shape it had when it entered the viewport. Harmless.");
        return false;
    }

    gWindow = window;
    gPreviousProc = (WNDPROC) previous;
    return true;
}

void Detach ()
{
    if (gWindow == nullptr) {
        gPreviousProc = nullptr;
        return;
    }
    // ⚠️ ONLY IF THE WINDOW IS STILL ALIVE, and only if WE are still the proc.
    // Restoring over a window someone else subclassed after us would tear out
    // their proc as well as ours.
    if (::IsWindow (gWindow) != FALSE && gPreviousProc != nullptr) {
        WNDPROC const current = (WNDPROC) ::GetWindowLongPtr (gWindow, GWLP_WNDPROC);
        if (current == &ViewportProc)
            ::SetWindowLongPtr (gWindow, GWLP_WNDPROC, (LONG_PTR) gPreviousProc);
    }
    gWindow = nullptr;
    gPreviousProc = nullptr;
}

}   // namespace viewportcursor
}   // namespace archviz
}   // namespace geomsrv
