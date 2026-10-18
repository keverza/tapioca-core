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
int s_hostMiddleDown = 0, s_hostMiddleUp = 0;
int s_hostNavigationMoves = 0;
constexpr UINT kHostCursorMessage = WM_APP + 43;

LRESULT CALLBACK HostProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kHostCursorMessage) {
        ::SetCursor (::LoadCursorW (nullptr, MAKEINTRESOURCEW (32515)));
        return 0;
    }
    if (message == WM_MOUSEHOVER)
        ++s_hostHovers;
    if (message == WM_MOUSELEAVE)
        ++s_hostLeaves;
    if (message == WM_COMMAND && LOWORD (wParam) == 4242)
        ++s_hostDeletes;
    if (message == WM_MBUTTONDOWN)
        ++s_hostMiddleDown;
    if (message == WM_MBUTTONUP)
        ++s_hostMiddleUp;
    if (message == WM_MOUSEMOVE && (wParam & MK_MBUTTON) != 0)
        ++s_hostNavigationMoves;
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

TEST (OverlayInput, CapturedEditorNavigationIsPassedAndReported)
{
    const HWND canvas = Canvas ();
    ASSERT_NE (canvas, nullptr);
    std::string error;
    ASSERT_TRUE (input::Attach (input::View::Plan, canvas, Owner (), error)) << error;
    input::HitMap map;
    map.locked = true;
    input::SetHitMap (input::View::Plan, map);
    Pump ();
    s_hostMiddleDown = s_hostMiddleUp = 0;
    s_hostNavigationMoves = 0;
    ::PostMessageW (canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (40, 40));
    Pump ();
    auto events = input::TakeInput (input::View::Plan);
    ASSERT_EQ (events.buttons.size (), 1u);
    EXPECT_EQ (events.buttons[0].button, 0);
    ::PostMessageW (canvas, WM_MBUTTONDOWN, MK_LBUTTON | MK_MBUTTON, MAKELPARAM (40, 40));
    Pump ();
    events = input::TakeInput (input::View::Plan);
    EXPECT_TRUE (events.navigating);
    EXPECT_EQ (events.navigationRevision, 1u);
    EXPECT_TRUE (events.buttons.empty ());
    EXPECT_EQ (s_hostMiddleDown, 1);
    ::PostMessageW (canvas, WM_MOUSEMOVE, MK_LBUTTON | MK_MBUTTON, MAKELPARAM (50, 40));
    Pump ();
    EXPECT_EQ (s_hostNavigationMoves, 1);
    EXPECT_TRUE (input::TakeInput (input::View::Plan).buttons.empty ());
    ::PostMessageW (canvas, WM_LBUTTONUP, MK_MBUTTON, MAKELPARAM (50, 40));
    Pump ();
    events = input::TakeInput (input::View::Plan);
    ASSERT_EQ (events.buttons.size (), 1u);
    EXPECT_FALSE (events.buttons[0].down);
    EXPECT_TRUE (events.navigating);
    ::PostMessageW (canvas, WM_MBUTTONUP, 0, MAKELPARAM (50, 40));
    ::PostMessageW (canvas, WM_MOUSEWHEEL, MAKEWPARAM (0, WHEEL_DELTA), MAKELPARAM (50, 40));
    Pump ();
    events = input::TakeInput (input::View::Plan);
    EXPECT_FALSE (events.navigating);
    EXPECT_EQ (events.navigationRevision, 3u);
    EXPECT_EQ (s_hostMiddleUp, 1);
    EXPECT_TRUE (events.buttons.empty ());
    EXPECT_NE (events.viewIdentity, 0u);
    // A scrollable HUD page still consumes its wheel; it is not a native zoom event.
    input::Region page;
    page.rect[0] = page.rect[1] = -1000000;
    page.rect[2] = page.rect[3] = 1000000;
    page.scrolls = true;
    map.regions.push_back (page);
    input::SetHitMap (input::View::Plan, map);
    ::PostMessageW (canvas, WM_MOUSEWHEEL, MAKEWPARAM (0, WHEEL_DELTA), MAKELPARAM (50, 40));
    Pump ();
    events = input::TakeInput (input::View::Plan);
    EXPECT_EQ (events.navigationRevision, 3u);
    EXPECT_FLOAT_EQ (events.wheel, 1.0f);
    input::Detach (input::View::Plan);
    ::DestroyWindow (canvas);
    Pump ();
}

// ⚠️ THE HUD'S CURSOR DOES NOT DEPEND ON THE NATIVE GESTURE'S RELEASE HAVING BEEN SEEN
// (2026-10-18). The published cursor is the first thing the user sees change, and the virtual
// editor's flickered "for seconds after" an interaction. Archicad's middle-button pan owns the
// SHOWN cursor while it is held, and the state that said so was an edge latch set by a passed
// middle press and cleared only when a later event was weighed -- so a release no event carried
// (the button came up outside every window, or WH_MOUSE refused, which the code warns for) left
// the HUD unable to put its cursor back at all, until the pointer moved again. The hold is now
// read live, so the very next message the thread takes restores it with no pointer motion at all.
TEST (OverlayInput, TheHudsCursorIsRestoredWithoutTheNavigationReleaseBeingSeen)
{
    const HMODULE controls = ::LoadLibraryW (L"comctl32.dll"); // the canvas subclass answers WM_SETCURSOR
    ASSERT_NE (controls, nullptr);
    // ⚠️ THE CURSORS BY EXPLICIT WIDE IDS: this test build has no UNICODE `IDC_*` (the macros expand
    // to `MAKEINTRESOURCEA`), so they do not type as `LPCWSTR` for `LoadCursorW`. 32512 is
    // `IDC_ARROW`, 32515 is `IDC_CROSS` (winuser.h).
    const HCURSOR arrow = ::LoadCursorW (nullptr, MAKEINTRESOURCEW (32512));
    const HCURSOR cross = ::LoadCursorW (nullptr, MAKEINTRESOURCEW (32515));
    ASSERT_NE (arrow, nullptr);
    ASSERT_NE (cross, nullptr);
    const HWND canvas = Canvas (), other = Canvas ();
    ASSERT_NE (canvas, nullptr);
    std::string error;
    ASSERT_TRUE (input::Attach (input::View::Plan, canvas, Owner (), error)) << error;
    input::HitMap map;
    map.locked = true;
    input::SetHitMap (input::View::Plan, map);
    Pump ();
    ASSERT_TRUE (input::GetStats ().subclassed[1]) << "the canvas must be the one that shows the cursor";

    // A HUD gesture holds the pointer, then a native middle-button pan runs through it: the pan
    // is passed to Archicad, the HUD's own press is not (`NavigationPassesDuringACapturedHudGesture`).
    ::PostMessageW (canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (40, 40));
    ::PostMessageW (canvas, WM_MBUTTONDOWN, MK_LBUTTON | MK_MBUTTON, MAKELPARAM (40, 40));
    ::PostMessageW (canvas, WM_MOUSEMOVE, MK_LBUTTON | MK_MBUTTON, MAKELPARAM (50, 40));
    Pump ();
    // The release is never delivered: the button is up -- nothing physical is held -- and the next
    // message the thread takes belongs to another window, as Archicad's own messages do.
    ::SetCursor (cross); // what Archicad left on the pan
    ::PostMessageW (other, WM_NULL, 0, 0);
    Pump ();
    EXPECT_EQ (::GetCursor (), arrow) << "the HUD's cursor comes back at the next message, with no next move";

    // And with the release itself delivered, it is back on that message -- again without motion.
    ::PostMessageW (canvas, WM_MBUTTONDOWN, MK_LBUTTON | MK_MBUTTON, MAKELPARAM (50, 40));
    Pump ();
    ::SetCursor (cross);
    ::PostMessageW (canvas, WM_MBUTTONUP, MK_LBUTTON, MAKELPARAM (50, 40));
    Pump ();
    EXPECT_EQ (::GetCursor (), arrow) << "the release message is enough";

    input::Shutdown ();
    ::DestroyWindow (other);
    ::DestroyWindow (canvas);
    ::FreeLibrary (controls);
}

TEST (OverlayInput, TheHudsCursorWinsAfterAnotherWindowDispatches)
{
    const HMODULE controls = ::LoadLibraryW (L"comctl32.dll");
    ASSERT_NE (controls, nullptr);
    const HWND canvas = Canvas (), other = Canvas ();
    ASSERT_NE (canvas, nullptr);
    ASSERT_NE (other, nullptr);
    std::string error;
    ASSERT_TRUE (input::Attach (input::View::Plan, canvas, Owner (), error)) << error;
    input::HitMap map;
    map.locked = true;
    input::SetHitMap (input::View::Plan, map);
    Pump ();
    // Capture belongs to the HUD. A different native window then overwrites its cursor
    // inside dispatch, after WH_GETMESSAGE has already run. No next message repairs it.
    ::PostMessageW (canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (40, 40));
    Pump ();
    ::SendMessageW (other, kHostCursorMessage, 0, 0);
    EXPECT_EQ (::GetCursor (), ::LoadCursorW (nullptr, MAKEINTRESOURCEW (32512)))
        << "a sent native message is repaired before its caller regains control";
    ::PostMessageW (other, kHostCursorMessage, 0, 0);
    Pump ();
    EXPECT_EQ (::GetCursor (), ::LoadCursorW (nullptr, MAKEINTRESOURCEW (32512)));
    input::Shutdown ();
    ::SendMessageW (other, kHostCursorMessage, 0, 0);
    EXPECT_EQ (::GetCursor (), ::LoadCursorW (nullptr, MAKEINTRESOURCEW (32515)))
        << "shutdown removes the thread-local post-dispatch cursor hook";
    ::DestroyWindow (other);
    ::DestroyWindow (canvas);
    ::FreeLibrary (controls);
}

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
