// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- like the input hook it serves, this runs inside
// Archicad's message dispatch: it decides and returns, it never calls ACAPI.
// ArchViz/OverlayInputCursor -- the cursor over the HUD: which one, shown when, and kept.

#include "ArchViz/OverlayInputEvents.hpp"
#include "ArchViz/OverlayInputState.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <commctrl.h>

namespace geomsrv::archviz::overlayinput::state {

namespace {
using events::PhysicalButtons;

// ---- the cursor over the HUD ------------------------------------------------------------
// Resolved from the comctl32 Archicad has already loaded, as PlanFrameSession's: the add-on
// gains no import for it.
using SetSubclassFn = BOOL (WINAPI*) (HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
using RemoveSubclassFn = BOOL (WINAPI*) (HWND, SUBCLASSPROC, UINT_PTR);
using DefSubclassFn = LRESULT (WINAPI*) (HWND, UINT, WPARAM, LPARAM);
SetSubclassFn g_setSubclass = nullptr;
RemoveSubclassFn g_removeSubclass = nullptr;
DefSubclassFn g_defSubclass = nullptr;
// One id per view: removing one view's never removes the other's.
constexpr UINT_PTR kSubclassId = 0x54485544; // 'THUD'
HCURSOR g_arrowCursor = nullptr;
HCURSOR g_handCursor = nullptr;
HCURSOR g_moveCursor = nullptr;
HCURSOR g_sizeWECursor = nullptr;
HCURSOR g_sizeNSCursor = nullptr;
HCURSOR s_textCursor = nullptr;
HCURSOR g_crossCursor = nullptr;

bool ResolveSubclassing ()
{
    if (g_setSubclass != nullptr && g_removeSubclass != nullptr && g_defSubclass != nullptr)
        return true;
    const HMODULE comctl = ::GetModuleHandleW (L"comctl32.dll");
    if (comctl == nullptr)
        return false;
    g_setSubclass = reinterpret_cast<SetSubclassFn> (::GetProcAddress (comctl, "SetWindowSubclass"));
    g_removeSubclass = reinterpret_cast<RemoveSubclassFn> (::GetProcAddress (comctl, "RemoveWindowSubclass"));
    g_defSubclass = reinterpret_cast<DefSubclassFn> (::GetProcAddress (comctl, "DefSubclassProc"));
    if (g_setSubclass != nullptr && g_removeSubclass != nullptr && g_defSubclass != nullptr)
        return true;
    g_setSubclass = nullptr;
    g_removeSubclass = nullptr;
    g_defSubclass = nullptr;
    return false;
}

} // namespace

bool OwnsPointer (const Target& target)
{
    if (target.owner.shown == nullptr || !target.owner.shown ())
        return false;
    if (router.GetOwner () != Owner::None)
        return router.GetOwner () == Owner::Hud;
    POINT screen = {};
    if (!::GetCursorPos (&screen))
        return false;
    const HWND under = ::WindowFromPoint (screen);
    if (under != target.canvas && !::IsChild (target.canvas, under))
        return false;
    POINT point = screen;
    RECT client = {};
    if (!::ScreenToClient (target.canvas, &point) || !::GetClientRect (target.canvas, &client))
        return false;
    return target.map.Owns (float (point.x), float (point.y), float (client.right - client.left),
                            float (client.bottom - client.top));
}

namespace {

// ⚠️ AN ARROW OVER THE HUD, A HAND OVER WHAT IT CAN PRESS (the user, 2026-09-29) -- never
// the tool's cursor Archicad would show over the model there. False when the pointer is
// not the HUD's: Archicad's cursor stands.
// The cursor the HUD asks for on `target` now: a canvas's own, else a hand on what it can press.
HCURSOR DesiredCursor (const Target& target, Cursor& desired)
{
    POINT point = {};
    RECT client = {};
    const bool overRegion =
        ::GetCursorPos (&point) && ::ScreenToClient (target.canvas, &point) &&
        ::GetClientRect (target.canvas, &client) &&
        target.map.Hit (float (point.x), float (point.y), float (client.right), float (client.bottom)) >= 0;
    desired = CursorFor (target.map, overRegion, router.GetOwner () == Owner::Hud && PhysicalButtons () != 0);
    HCURSOR cursor = g_arrowCursor;
    switch (desired) {
        case Cursor::Hand:
            cursor = g_handCursor;
            break;
        case Cursor::Move:
            cursor = g_moveCursor;
            break;
        case Cursor::SizeWE:
            cursor = g_sizeWECursor;
            break;
        case Cursor::SizeNS:
            cursor = g_sizeNSCursor;
            break;
        case Cursor::Text:
            cursor = s_textCursor;
            break;
        case Cursor::Cross:
            cursor = g_crossCursor;
            break;
        default:
            break;
    }
    return cursor != nullptr ? cursor : g_arrowCursor;
}

} // namespace

bool ShowCursor (const Target& target)
{
    if (!OwnsPointer (target))
        return false;
    Cursor desired = Cursor::Arrow;
    ::SetCursor (DesiredCursor (target, desired));
    ++stats.cursorsSet;
    if (desired == Cursor::Hand)
        ++stats.handsShown;
    return true;
}

// ⚠️ ARCHICAD SETS ITS TOOL CURSOR OUTSIDE WM_SETCURSOR TOO (the user, 2026-10-14: on the locked
// view the virtual editor's cursor flickered between the arrow and a crosshair). After anything
// the canvas handled, a locked view whose pointer is the HUD's gets the HUD's cursor back --
// counted, so a live run says whether, and how often, Archicad replaced it.
void ReassertCursor (const Target& target)
{
    if (!target.map.locked || !OwnsPointer (target))
        return;
    Cursor desired = Cursor::Arrow;
    const HCURSOR cursor = DesiredCursor (target, desired);
    if (::GetCursor () == cursor)
        return;
    ::SetCursor (cursor);
    ++stats.cursorsReasserted;
}

namespace {

LRESULT CALLBACK CanvasProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    if (message == WM_NCDESTROY) {
        // The canvas is going: the subclass goes with it.
        g_removeSubclass (window, &CanvasProc, id);
        for (Target& target : targets)
            if (target.canvas == window)
                target.subclassed = false;
        return g_defSubclass (window, message, wParam, lParam);
    }
    if (message == WM_SETCURSOR && LOWORD (lParam) == HTCLIENT) {
        const Target* const target = TargetFor (window);
        if (target != nullptr && ShowCursor (*target))
            return TRUE;
    }
    if (message == WM_MOUSEHOVER) {
        const Target* const target = TargetFor (window);
        if (target != nullptr && target->map.locked && target->owner.shown != nullptr && target->owner.shown ())
            return 0; // a stationary native preselection/info-tag timer has no role on the locked canvas
    }
    // ⚠️ AND NO CONTEXT MENU OF ARCHICAD'S OVER THE HUD, however it was asked for: a right
    // click there is the HUD's (its own menu). The keyboard's (lParam -1) is Archicad's.
    if (message == WM_CONTEXTMENU && lParam != LPARAM (-1)) {
        const Target* const target = TargetFor (window);
        if (target != nullptr && OwnsPointer (*target)) {
            ++stats.contextMenusSwallowed;
            return 0;
        }
    }
    const LRESULT result = g_defSubclass (window, message, wParam, lParam);
    if (message != WM_SETCURSOR)
        if (const Target* const target = TargetFor (window); target != nullptr)
            ReassertCursor (*target);
    return result;
}

} // namespace

// ⚠️ ONLY A CANVAS OF THIS THREAD. A window is subclassed from its own thread or not at
// all -- and one of another thread posts its mouse messages to that thread's queue,
// where the hook never sees them: said once, and counted.
void Subclass (Target& target, View view)
{
    DWORD process = 0;
    target.ownThread = ::GetWindowThreadProcessId (target.canvas, &process) == ::GetCurrentThreadId ();
    if (!target.ownThread) {
        ArchVizLog (std::string ("OVERLAY INPUT  the ") + ViewName (view) +
                    " canvas belongs to another thread: the HUD sees none of its mouse messages");
        return;
    }
    if (!ResolveSubclassing ()) {
        ArchVizLog ("OVERLAY INPUT  comctl32 has no SetWindowSubclass here: Archicad's cursor stays over the HUD");
        return;
    }
    if (g_arrowCursor == nullptr)
        g_arrowCursor = ::LoadCursorW (nullptr, IDC_ARROW);
    if (g_handCursor == nullptr)
        g_handCursor = ::LoadCursorW (nullptr, IDC_HAND);
    if (g_moveCursor == nullptr)
        g_moveCursor = ::LoadCursorW (nullptr, IDC_SIZEALL);
    if (g_sizeWECursor == nullptr)
        g_sizeWECursor = ::LoadCursorW (nullptr, IDC_SIZEWE);
    if (g_sizeNSCursor == nullptr)
        g_sizeNSCursor = ::LoadCursorW (nullptr, IDC_SIZENS);
    if (!s_textCursor)
        s_textCursor = ::LoadCursorW (nullptr, IDC_IBEAM);
    if (g_crossCursor == nullptr)
        g_crossCursor = ::LoadCursorW (nullptr, IDC_CROSS);
    target.subclassed = g_setSubclass (target.canvas, &CanvasProc, kSubclassId + UINT_PTR (view), 0) != FALSE;
    if (!target.subclassed)
        ArchVizLog ("OVERLAY INPUT  SetWindowSubclass refused the canvas: Archicad's cursor stays over the HUD");
}

void Unsubclass (Target& target)
{
    if (target.subclassed && target.canvas != nullptr && ::IsWindow (target.canvas) && g_removeSubclass != nullptr)
        g_removeSubclass (target.canvas, &CanvasProc, kSubclassId + UINT_PTR (ViewOf (target)));
    target.subclassed = false;
}

void ForgetSubclassing ()
{
    g_setSubclass = nullptr, g_removeSubclass = nullptr, g_defSubclass = nullptr;
}

} // namespace geomsrv::archviz::overlayinput::state
