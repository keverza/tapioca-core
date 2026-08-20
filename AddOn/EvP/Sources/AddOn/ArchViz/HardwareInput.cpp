#include "ArchViz/HardwareInput.hpp"

#include <windows.h>

namespace geomsrv {
namespace archviz {

namespace {

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

}   // namespace

void PollHardwareInput (void* nwh, InputSnapshot& io)
{
    HWND const hwnd = (HWND) nwh;
    POINT screenPt = {};
    if (hwnd == nullptr || ::GetCursorPos (&screenPt) == FALSE)
        return;

    POINT clientPt = screenPt;
    if (::ScreenToClient (hwnd, &clientPt) == FALSE)
        return;

    io.x = int32_t (clientPt.x);
    io.y = int32_t (clientPt.y);
    io.inside = CursorIsOverUs (hwnd, screenPt, clientPt);
    // The high bit is "down now"; the low bit is "pressed since the last call"
    // and is explicitly NOT wanted -- it is per-thread and would report a press
    // this thread never saw.
    io.shift = (::GetAsyncKeyState (VK_SHIFT) & 0x8000) != 0;
    io.navButton = (::GetAsyncKeyState (VK_MBUTTON) & 0x8000) != 0;
}

}   // namespace archviz
}   // namespace geomsrv
