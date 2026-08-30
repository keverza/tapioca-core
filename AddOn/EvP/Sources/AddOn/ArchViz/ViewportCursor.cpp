#include "ArchViz/ViewportCursor.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/InputRingBuffer.hpp"

#include <windows.h>
#include <windowsx.h>

namespace geomsrv {
namespace archviz {
namespace viewportcursor {

namespace {

HWND gWindow = nullptr;
WNDPROC gPreviousProc = nullptr;
HHOOK gMessageHook = nullptr;
bool gExpectedCaptureRelease = false;

bool PointTargetsViewport (POINT screenPoint)
{
    if (gWindow == nullptr || ::IsWindow (gWindow) == FALSE)
        return false;
    POINT clientPoint = screenPoint;
    RECT client = {};
    if (!::ScreenToClient (gWindow, &clientPoint) || !::GetClientRect (gWindow, &client) ||
        !::PtInRect (&client, clientPoint))
        return false;
    const HWND under = ::WindowFromPoint (screenPoint);
    return under != nullptr && ::GetAncestor (under, GA_ROOT) == ::GetAncestor (gWindow, GA_ROOT);
}

void ReleaseExpectedCapture (HWND hwnd)
{
    if (::GetCapture () != hwnd)
        return;
    gExpectedCaptureRelease = true;
    ::ReleaseCapture ();
    gExpectedCaptureRelease = false;
}

LRESULT CALLBACK MessageHook (int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && wParam == PM_REMOVE && gWindow != nullptr) {
        MSG* message = reinterpret_cast<MSG*> (lParam);
        if (message != nullptr && message->message == WM_MOUSEWHEEL && PointTargetsViewport (message->pt)) {
            InputRingBuffer::Get ().PushWheel (GET_WHEEL_DELTA_WPARAM (message->wParam));
            message->message = WM_NULL;
        }
    }
    return ::CallNextHookEx (gMessageHook, code, wParam, lParam);
}

LRESULT CALLBACK ViewportProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    InputRingBuffer& input = InputRingBuffer::Get ();
    switch (message) {
        case WM_MOUSEMOVE:
            input.PushPointerMessage (GET_X_LPARAM (lParam), GET_Y_LPARAM (lParam));
            break;
        case WM_LBUTTONDOWN:
            ::SetCapture (hwnd);
            input.PushButton (kMouseLeft, true);
            return 0;
        case WM_LBUTTONUP:
            input.PushButton (kMouseLeft, false);
            if ((wParam & (MK_LBUTTON | MK_RBUTTON)) == 0 && ::GetCapture () == hwnd)
                ReleaseExpectedCapture (hwnd);
            return 0;
        case WM_RBUTTONDOWN:
            ::SetCapture (hwnd);
            input.PushButton (kMouseRight, true);
            return 0;
        case WM_RBUTTONUP:
            input.PushButton (kMouseRight, false);
            if ((wParam & (MK_LBUTTON | MK_RBUTTON)) == 0 && ::GetCapture () == hwnd)
                ReleaseExpectedCapture (hwnd);
            return 0;
        case WM_MOUSEWHEEL:
            if (PointTargetsViewport ({ GET_X_LPARAM (lParam), GET_Y_LPARAM (lParam) })) {
                input.PushWheel (GET_WHEEL_DELTA_WPARAM (wParam));
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (!gExpectedCaptureRelease) {
                input.PushButton (kMouseLeft, false);
                input.PushButton (kMouseRight, false);
            }
            break;
        case WM_CANCELMODE:
            input.PushButton (kMouseLeft, false);
            input.PushButton (kMouseRight, false);
            break;
        default:
            break;
    }

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

} // namespace

bool Attach (void* hwnd)
{
    HWND const window = (HWND) hwnd;
    if (window == nullptr || ::IsWindow (window) == FALSE)
        return false;

    if (gWindow == window && gPreviousProc != nullptr)
        return true; // already ours

    // A different window than last time: put the old one back first, so a
    // reopened viewport cannot leave a subclass pointing at a dead HWND.
    if (gWindow != nullptr)
        Detach ();

    ::SetLastError (0);
    LONG_PTR const previous = ::SetWindowLongPtr (window, GWLP_WNDPROC, (LONG_PTR) &ViewportProc);
    if (previous == 0 && ::GetLastError () != 0) {
        ArchVizLog ("viewport cursor: SetWindowLongPtr failed; the pointer will keep "
                    "whatever shape it had when it entered the viewport. Harmless.");
        return false;
    }

    gWindow = window;
    gPreviousProc = (WNDPROC) previous;
    const DWORD threadId = ::GetWindowThreadProcessId (window, nullptr);
    gMessageHook = ::SetWindowsHookExW (WH_GETMESSAGE, &MessageHook, nullptr, threadId);
    if (gMessageHook == nullptr)
        ArchVizLog ("viewport input: WH_GETMESSAGE wheel routing failed; wheel requires child focus");
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
        if (::GetCapture () == gWindow)
            ReleaseExpectedCapture (gWindow);
        WNDPROC const current = (WNDPROC)::GetWindowLongPtr (gWindow, GWLP_WNDPROC);
        if (current == &ViewportProc)
            ::SetWindowLongPtr (gWindow, GWLP_WNDPROC, (LONG_PTR) gPreviousProc);
    }
    if (gMessageHook != nullptr) {
        ::UnhookWindowsHookEx (gMessageHook);
        gMessageHook = nullptr;
    }
    gWindow = nullptr;
    gPreviousProc = nullptr;
    InputRingBuffer::Get ().Reset ();
}

} // namespace viewportcursor
} // namespace archviz
} // namespace geomsrv
