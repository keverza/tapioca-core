// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- like the input hook it serves, this runs inside
// Archicad's message dispatch: it decides and returns, it never calls ACAPI.
// ArchViz/OverlayInputCursor -- the cursor over the HUD: which one, shown when, and kept.

#include "ArchViz/OverlayInputEvents.hpp"
#include "ArchViz/OverlayInputState.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace geomsrv::archviz::overlayinput::state {

namespace {
using events::PhysicalButtons;

// WindowFromPoint sends WM_NCHITTEST. Both the canvas subclass and the thread's
// return hook can run inside that query; neither may start another cursor query.
// Guard the query itself, including ownership checks made outside cursor repair.
bool s_queryingCursor = false;
struct CursorQuery {
    bool previous = s_queryingCursor;
    CursorQuery ()
    {
        s_queryingCursor = true;
    }
    ~CursorQuery ()
    {
        s_queryingCursor = previous;
    }
};

// ⚠️ THE NATIVE MIDDLE-BUTTON GESTURE OWNS THE SHOWN CURSOR WHILE IT IS HELD -- READ LIVE,
// NEVER FROM A LATCH (2026-10-18). Archicad pans and zooms with the middle button, and its
// gesture is not the HUD's: while it is held the HUD leaves the shown cursor alone. The state
// that said so used to be `Target::navigating`, an edge latch set by a passed middle press and
// cleared only when some later event was weighed -- so a release the hook never saw (the button
// went up outside every window, or WH_MOUSE was refused) left the HUD unable to put its cursor
// back at all, and `ReassertCursor` could not repair the message it was called for because the
// latch it read was still one event behind. The physical button cannot be behind: it is false
// the moment it is released, on the same message. `Target::navigating` stays the editing signal
// the HUD publishes (`overlayhud::Input::navigating`); it just no longer licenses the cursor.
bool NativeNavigation ()
{
    return (PhysicalButtons () & Bit (Button::Middle)) != 0;
}

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
HCURSOR g_penCursor = nullptr, g_penCloseCursor = nullptr;

// A pen's nib, drawn here: Windows has no pen cursor and the add-on ships no cursor files.
// Ink on a white halo, so it reads on the white plan and on a dark 3D view; the hot spot is
// the nib's tip. `close` adds a small ring: a click there closes the shape.
HCURSOR MakePenCursor (bool close)
{
    constexpr int kSize = 32;
    // The nib, tip at the lower left, in cursor pixels.
    const float nib[][2] = { { 2, 29 }, { 6, 17 }, { 18, 5 }, { 26, 13 }, { 14, 25 } };
    constexpr int kCorners = 5;
    const auto inside = [&] (float x, float y) {
        bool in = false;
        for (int i = 0, j = kCorners - 1; i < kCorners; j = i++)
            if ((nib[i][1] > y) != (nib[j][1] > y) &&
                x < (nib[j][0] - nib[i][0]) * (y - nib[i][1]) / (nib[j][1] - nib[i][1]) + nib[i][0])
                in = !in;
        return in;
    };
    const auto toSegment = [] (float px, float py, float ax, float ay, float bx, float by) {
        const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        return std::hypot (px - ax - dx * t, py - ay - dy * t);
    };
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof header;
    header.bV5Width = kSize;
    header.bV5Height = -kSize;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000, header.bV5GreenMask = 0x0000FF00, header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = ::GetDC (nullptr);
    HBITMAP colour =
        ::CreateDIBSection (screen, reinterpret_cast<BITMAPINFO*> (&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC (nullptr, screen);
    if (colour == nullptr || bits == nullptr)
        return nullptr;
    auto* px = static_cast<uint32_t*> (bits);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            float edge = 1e9f;
            for (int i = 0; i < kCorners; ++i)
                edge = std::min (edge, toSegment (fx, fy, nib[i][0], nib[i][1], nib[(i + 1) % kCorners][0],
                                                  nib[(i + 1) % kCorners][1]));
            const float slit = toSegment (fx, fy, 3, 28, 12, 19);               // the nib's slit
            const float ring = std::abs (std::hypot (fx - 24, fy - 26) - 3.5f); // the closing ring
            uint32_t p = 0;
            if (edge < 2.6f || (close && ring < 2.4f))
                p = 0xFFFFFFFFu; // halo
            if (inside (fx, fy))
                p = 0xFFFFFFFFu; // the nib's body
            if (edge < 1.1f || slit < 0.8f || (close && ring < 0.9f))
                p = 0xFF1E2228u; // ink
            px[y * kSize + x] = p;
        }
    HBITMAP mask = ::CreateBitmap (kSize, kSize, 1, 1, nullptr);
    ICONINFO info = {};
    info.fIcon = FALSE;
    info.xHotspot = 2;
    info.yHotspot = 29;
    info.hbmMask = mask;
    info.hbmColor = colour;
    HCURSOR cursor = ::CreateIconIndirect (&info);
    ::DeleteObject (colour);
    ::DeleteObject (mask);
    return cursor;
}

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
    const CursorQuery query;
    if (target.owner.shown == nullptr || !target.owner.shown ())
        return false;
    const Owner owner = CursorOwner (router.GetOwner (), PhysicalButtons ());
    if (owner != Owner::None)
        return owner == Owner::Hud;
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
        case Cursor::Pen:
            cursor = g_penCursor;
            break;
        case Cursor::PenClose:
            cursor = g_penCloseCursor;
            break;
        default:
            break;
    }
    return cursor != nullptr ? cursor : g_arrowCursor;
}

} // namespace

bool ShowCursor (const Target& target)
{
    if (s_queryingCursor)
        return false;
    const CursorQuery query;
    if (NativeNavigation () || !OwnsPointer (target))
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
// ⚠️ AND IT COMES BACK AT THE RELEASE ITSELF, NOT AT THE POINTER'S NEXT MOVE (2026-10-18): this
// runs for every message the thread takes, and the native gesture's hold is read live
// (`NativeNavigation`), so the middle button's own release message is enough to put the HUD's
// cursor back with no further input of any kind.
void ReassertCursor (const Target& target)
{
    if (s_queryingCursor)
        return;
    const CursorQuery query;
    if (NativeNavigation () || !target.map.locked || !OwnsPointer (target))
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
    if (g_penCursor == nullptr)
        g_penCursor = MakePenCursor (false);
    if (g_penCloseCursor == nullptr)
        g_penCloseCursor = MakePenCursor (true);
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
    // The drawn cursors belong to this module: none outlives its session.
    for (HCURSOR* drawn : { &g_penCursor, &g_penCloseCursor })
        if (*drawn != nullptr)
            ::DestroyIcon (*drawn), *drawn = nullptr;
}

} // namespace geomsrv::archviz::overlayinput::state
