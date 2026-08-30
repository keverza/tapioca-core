#include "ArchViz/HardwareInput.hpp"

#include <windows.h>

#include <mutex>

namespace geomsrv {
namespace archviz {

namespace {

std::mutex s_inputGateMutex;
void* s_inputGateWindow = nullptr;
bool s_inputGateEnabled = true;

// Rect containment AND nothing covering us. See the header for what the rect
// test alone let through.
bool CursorIsOverUs (HWND hwnd, POINT screenPt, POINT clientPt)
{
    RECT client = {};
    ::GetClientRect (hwnd, &client);
    if (clientPt.x < 0 || clientPt.y < 0 || clientPt.x >= client.right || clientPt.y >= client.bottom)
        return false;

    // WindowFromPoint returns the visible, enabled window at that point --
    // whatever is on TOP. Comparing GA_ROOT rather than the handle itself
    // because it may legitimately hand back our palette frame, a sibling DG
    // item, or the viewport itself depending on where in the rect the cursor is;
    // all three mean "our palette is the window in front here", and the rect
    // test above has already established WHERE in it.
    HWND const under = ::WindowFromPoint (screenPt);
    if (under == nullptr)
        return false;
    return ::GetAncestor (under, GA_ROOT) == ::GetAncestor (hwnd, GA_ROOT);
}

bool HardwareInputEnabled (void* nwh)
{
    std::lock_guard<std::mutex> lock (s_inputGateMutex);
    return s_inputGateWindow != nwh || s_inputGateEnabled;
}

} // namespace

bool ReadHardwarePointer (void* nwh, HardwarePointerPosition& position)
{
    const HWND hwnd = static_cast<HWND> (nwh);
    POINT screenPt = {};
    if (hwnd == nullptr || ::IsWindow (hwnd) == FALSE || ::GetCursorPos (&screenPt) == FALSE)
        return false;

    POINT clientPt = screenPt;
    if (::ScreenToClient (hwnd, &clientPt) == FALSE)
        return false;

    position.x = int32_t (clientPt.x);
    position.y = int32_t (clientPt.y);
    position.screenX = int32_t (screenPt.x);
    position.screenY = int32_t (screenPt.y);
    position.inside = CursorIsOverUs (hwnd, screenPt, clientPt);
    return true;
}

void SetHardwareInputEnabled (void* nwh, bool enabled)
{
    if (nwh == nullptr)
        return;
    std::lock_guard<std::mutex> lock (s_inputGateMutex);
    s_inputGateWindow = nwh;
    s_inputGateEnabled = enabled;
}

void ForgetHardwareInputWindow (void* nwh)
{
    std::lock_guard<std::mutex> lock (s_inputGateMutex);
    if (s_inputGateWindow == nwh) {
        s_inputGateWindow = nullptr;
        s_inputGateEnabled = true;
    }
}

void PollHardwareInput (void* nwh, InputSnapshot& io)
{
    static thread_local uint64_t pointerSequence = 0;
    HardwarePointerPosition pointer;
    if (!ReadHardwarePointer (nwh, pointer))
        return;

    io.x = pointer.x;
    io.y = pointer.y;
    io.screenX = pointer.screenX;
    io.screenY = pointer.screenY;
    RECT client = {};
    const HWND hwnd = static_cast<HWND> (nwh);
    if (::GetClientRect (hwnd, &client)) {
        io.clientWidth = client.right - client.left;
        io.clientHeight = client.bottom - client.top;
    }
    io.dpi = ::GetDpiForWindow (hwnd);
    io.visible = ::IsWindowVisible (hwnd) != FALSE;
    const HWND root = ::GetAncestor (hwnd, GA_ROOT);
    io.hostMinimized = root != nullptr && ::IsIconic (root) != FALSE;
    if (root != nullptr) {
        RECT hostRect = {};
        if (::GetWindowRect (root, &hostRect)) {
            io.hostX = hostRect.left;
            io.hostY = hostRect.top;
            io.hostWidth = hostRect.right - hostRect.left;
            io.hostHeight = hostRect.bottom - hostRect.top;
        }
        io.hostStyle = uint64_t (::GetWindowLongPtr (root, GWL_STYLE));
        io.hostExStyle = uint64_t (::GetWindowLongPtr (root, GWL_EXSTYLE));
        io.hostToolWindow = (io.hostExStyle & WS_EX_TOOLWINDOW) != 0;
    }
    io.monitor = uint64_t (reinterpret_cast<uintptr_t> (::MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST)));
    LARGE_INTEGER sample = {};
    if (::QueryPerformanceCounter (&sample)) {
        io.pointerQpc = uint64_t (sample.QuadPart);
        io.pointerSequence = ++pointerSequence;
    }
    const bool enabled = HardwareInputEnabled (nwh);
    io.inside = enabled && pointer.inside;
    // The high bit is "down now"; the low bit is "pressed since the last call"
    // and is explicitly NOT wanted -- it is per-thread and would report a press
    // this thread never saw.
    io.shift = enabled && (::GetAsyncKeyState (VK_SHIFT) & 0x8000) != 0;
    io.navButton = enabled && (::GetAsyncKeyState (VK_MBUTTON) & 0x8000) != 0;
}

} // namespace archviz
} // namespace geomsrv
