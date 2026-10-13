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
int s_hostHovers = 0, s_hostLeaves = 0;
int s_hostDeletes = 0;

LRESULT CALLBACK HostProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_MOUSEHOVER)
        ++s_hostHovers;
    if (message == WM_MOUSELEAVE)
        ++s_hostLeaves;
    if (message == WM_COMMAND && LOWORD (wParam) == 4242)
        ++s_hostDeletes;
    return ::DefWindowProcW (window, message, wParam, lParam);
}

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
    wc.lpfnWndProc = &HostProc;
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

TEST (OverlayInput, RoofEscapeIsScopedToItsLockedCanvasAndStopsWithTheSession)
{
    const HWND canvas = Canvas (), other = Canvas ();
    std::string error;
    ASSERT_TRUE (input::Attach (input::View::Plan, canvas, Owner (), error));
    input::HitMap map;
    map.locked = true;
    input::SetHitMap (input::View::Plan, map);
    input::EnableRoofEditKeys (true);
    ::PostMessageW (other, WM_KEYDOWN, VK_ESCAPE, 0);
    Pump ();
    EXPECT_TRUE (input::TakeInput (input::View::Plan).keys.empty ());
    ::PostMessageW (canvas, WM_KEYDOWN, VK_ESCAPE, 0);
    Pump ();
    const auto taken = input::TakeInput (input::View::Plan);
    ASSERT_EQ (taken.keys.size (), 1u);
    EXPECT_EQ (taken.keys.front ().code, uint32_t (VK_ESCAPE));
    ::PostMessageW (canvas, WM_KEYDOWN, VK_DELETE, 0);
    Pump ();
    const auto blocked = input::TakeInput (input::View::Plan);
    ASSERT_EQ (blocked.keys.size (), 1u);
    EXPECT_EQ (blocked.keys.front ().code, uint32_t (VK_DELETE));
    input::EnableRoofEditKeys (false);
    ::PostMessageW (canvas, WM_KEYDOWN, VK_ESCAPE, 0);
    Pump ();
    EXPECT_TRUE (input::TakeInput (input::View::Plan).keys.empty ());
    input::Shutdown ();
    ::DestroyWindow (other);
    ::DestroyWindow (canvas);
}

TEST (OverlayInput, LockedCanvasCancelsOldHoverAndRejectsStationaryHostFeedbackOnlyWhileLocked)
{
    const HMODULE controls = ::LoadLibraryW (L"comctl32.dll"); // Archicad loads this before its canvas is attached
    ASSERT_NE (controls, nullptr);
    const HWND canvas = Canvas (), other = Canvas ();
    std::string error;
    ASSERT_TRUE (input::Attach (input::View::Plan, canvas, Owner (), error));
    s_hostHovers = s_hostLeaves = 0;
    input::HitMap map;
    map.locked = true;
    input::SetHitMap (input::View::Plan, map);
    EXPECT_EQ (s_hostLeaves, 1) << "clear feedback already scheduled before the lock";
    ::SendMessageW (canvas, WM_MOUSEHOVER, 0, 0);
    EXPECT_EQ (s_hostHovers, 0);
    ::SendMessageW (other, WM_MOUSEHOVER, 0, 0);
    EXPECT_EQ (s_hostHovers, 1) << "other host windows are untouched";
    map.locked = false;
    input::SetHitMap (input::View::Plan, map);
    ::SendMessageW (canvas, WM_MOUSEHOVER, 0, 0);
    EXPECT_EQ (s_hostHovers, 2) << "normal host hover resumes when unlocked";
    input::Shutdown ();
    ::DestroyWindow (other);
    ::DestroyWindow (canvas);
    ::FreeLibrary (controls);
}
TEST (OverlayInput, LockedRoofConsumesParentDeleteBeforeNativeAcceleratorsAndForwardsText)
{
    const HWND parent = Canvas (), other = Canvas ();
    const HWND canvas = ::CreateWindowExW (0, L"TapiocaOverlayInputTestCanvas", L"", WS_CHILD, 0, 0, 400, 300, parent,
                                           nullptr, ::GetModuleHandleW (nullptr), nullptr);
    std::string error;
    ASSERT_TRUE (input::Attach (input::View::Plan, canvas, Owner (), error));
    input::HitMap map;
    map.locked = true;
    input::SetHitMap (input::View::Plan, map);
    ASSERT_TRUE (input::EnableRoofEditKeys (true));
    ACCEL shortcut { FVIRTKEY, VK_DELETE, 4242 };
    const HACCEL accelerators = ::CreateAcceleratorTableW (&shortcut, 1);
    s_hostDeletes = 0;
    auto pump = [&] (HWND recipient) {
        MSG message;
        while (::PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
            if (!::TranslateAcceleratorW (recipient, accelerators, &message)) {
                ::TranslateMessage (&message);
                ::DispatchMessageW (&message);
            }
    };
    ::PostMessageW (parent, WM_KEYDOWN, VK_DELETE, 0);
    ::PostMessageW (parent, WM_CHAR, L'3', 0);
    pump (parent);
    const auto taken = input::TakeInput (input::View::Plan);
    ASSERT_EQ (taken.keys.size (), 1u);
    EXPECT_EQ (taken.keys.front ().code, uint32_t (VK_DELETE));
    EXPECT_EQ (taken.characters, u"3");
    EXPECT_EQ (s_hostDeletes, 0);
    ::PostMessageW (other, WM_KEYDOWN, VK_DELETE, 0);
    pump (other);
    EXPECT_EQ (s_hostDeletes, 1) << "an unrelated native window retains its accelerator";
    map.locked = false;
    input::SetHitMap (input::View::Plan, map);
    ::PostMessageW (parent, WM_KEYDOWN, VK_DELETE, 0);
    pump (parent);
    EXPECT_EQ (s_hostDeletes, 2) << "unlocking restores native modelling keys";
    input::Shutdown ();
    ::DestroyAcceleratorTable (accelerators);
    ::DestroyWindow (parent);
    ::DestroyWindow (other);
}
