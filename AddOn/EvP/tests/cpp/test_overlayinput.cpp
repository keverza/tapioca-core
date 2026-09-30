// ArchViz/OverlayInput: the Win32 side of the HUD's input -- the hooks, the canvas it takes
// messages from, the layouts it asks its owner for. A plain window of the test's own thread
// stands in for Archicad's canvas; the owner counts what it is asked.

#include "ArchViz/OverlayInput.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <string>

namespace input = geomsrv::archviz::overlayinput;

namespace {

int g_refreshes = 0;

bool Shown ()
{
    return true;
}

bool Refresh ()
{
    ++g_refreshes;
    return false;
}

void Redraw ()
{
}

input::HudOwner Owner ()
{
    input::HudOwner owner;
    owner.shown = &Shown;
    owner.refresh = &Refresh;
    owner.redraw = &Redraw;
    return owner;
}

// A canvas: a hidden window of this thread, as Archicad's 3D and plan canvases are its main
// thread's.
HWND Canvas ()
{
    static const wchar_t kClass[] = L"TapiocaOverlayInputTestCanvas";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof (wc);
    wc.lpfnWndProc = &::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW (nullptr);
    wc.lpszClassName = kClass;
    ::RegisterClassExW (&wc);
    return ::CreateWindowExW (0, kClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr,
                              ::GetModuleHandleW (nullptr), nullptr);
}

// What the input layer posted to itself, handled, as Archicad's message loop would.
void Pump ()
{
    MSG message;
    while (::PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage (&message);
        ::DispatchMessageW (&message);
    }
}

} // namespace

// ⚠️ THE USER, 2026-09-30: after 3D -> plan -> 3D, and after the 3D window was closed and
// opened again, the HUD was drawn but static and took nothing. Detach forgets where the HUD
// is on the view; until a layout says it again every message passes to Archicad, and only a
// message the HUD took asked for a layout. A canvas newly taken -- the first time, again
// after a detach, or another one -- asks the owner for a layout; the same one again does not.
TEST (OverlayInput, ACanvasNewlyTakenAsksForALayout)
{
    const HWND canvas = Canvas ();
    ASSERT_NE (canvas, nullptr);
    g_refreshes = 0;
    std::string error;

    ASSERT_TRUE (input::Attach (input::View::ThreeD, canvas, Owner (), error)) << error;
    Pump ();
    EXPECT_EQ (g_refreshes, 1) << "laid out for the canvas it takes";
    ASSERT_TRUE (input::Attach (input::View::ThreeD, canvas, Owner (), error)) << error;
    Pump ();
    EXPECT_EQ (g_refreshes, 1) << "the same canvas: nothing new to lay out";

    // The view's overlay stopped, then started again on the same canvas.
    input::HitMap map;
    map.regions.emplace_back ();
    map.regions.back ().rect[2] = 100.0f;
    map.regions.back ().rect[3] = 100.0f;
    input::SetHitMap (input::View::ThreeD, map);
    EXPECT_EQ (input::GetStats ().regions[0], 1u);
    input::Detach (input::View::ThreeD);
    EXPECT_EQ (input::GetStats ().regions[0], 0u) << "where the HUD was goes with the session";
    ASSERT_TRUE (input::Attach (input::View::ThreeD, canvas, Owner (), error)) << error;
    Pump ();
    EXPECT_EQ (g_refreshes, 2) << "and a layout says it again";

    // Another canvas -- the 3D window closed and opened again.
    const HWND reopened = Canvas ();
    ASSERT_TRUE (input::Attach (input::View::ThreeD, reopened, Owner (), error)) << error;
    Pump ();
    EXPECT_EQ (g_refreshes, 3);

    input::Shutdown ();
    EXPECT_FALSE (input::GetStats ().installed) << "no hook left behind";
    ::DestroyWindow (reopened);
    ::DestroyWindow (canvas);
}
