#ifndef TAPIOCA_TESTS_QUIET_DESKTOP_HPP
#define TAPIOCA_TESTS_QUIET_DESKTOP_HPP

// The desktop the offline tests' input layer reads (ArchViz/OverlayInputDesktop.hpp): nobody at
// the desk. The pointer rests far from every test window, the cursor shows, no key or button is
// held, and the foreground window is none of the tests'. Installed for the whole run by
// quiet_desktop.cpp, so the run does not depend on what the person at the machine is doing.
#include "ArchViz/OverlayInputDesktop.hpp"

namespace hudtest {

inline geomsrv::archviz::overlayinput::Desktop QuietDesktop ()
{
    geomsrv::archviz::overlayinput::Desktop quiet;
    quiet.cursorPos = [] (LPPOINT point) -> BOOL {
        point->x = point->y = -30000;
        return TRUE;
    };
    quiet.windowFromPoint = [] (POINT) -> HWND { return nullptr; };
    quiet.cursorInfo = [] (PCURSORINFO info) -> BOOL {
        info->flags = CURSOR_SHOWING;
        info->hCursor = ::GetCursor ();
        info->ptScreenPos = { -30000, -30000 };
        return TRUE;
    };
    quiet.asyncKeyState = [] (int) -> SHORT { return 0; };
    quiet.keyState = [] (int) -> SHORT { return 0; };
    quiet.foregroundWindow = [] () -> HWND { return nullptr; };
    return quiet;
}

// The real desktop for one scope: a test whose input is the real pointer.
struct LiveDesktop {
    LiveDesktop ()
    {
        geomsrv::archviz::overlayinput::desktop = {};
    }
    ~LiveDesktop ()
    {
        geomsrv::archviz::overlayinput::desktop = QuietDesktop ();
    }
};

} // namespace hudtest
#endif
