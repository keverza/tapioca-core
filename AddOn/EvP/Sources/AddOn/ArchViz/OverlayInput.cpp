// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. This hook runs inside Archicad's message dispatch: it decides and
// returns, it never calls ACAPI, and it is gone before its session is (§8, §11).
// ArchViz/OverlayInput -- see the header.

#include "ArchViz/OverlayInput.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlayinput {

namespace {

struct Target {
    HWND canvas = nullptr;
    ShownTest shown = nullptr;
    HitMap map;
};

HHOOK g_hook = nullptr;
Target g_targets[2];
// One latch: the views never run together, and a gesture is one pointer's.
Router g_router;
Stats g_stats;

Target& TargetOf (View view)
{
    return g_targets[view == View::ThreeD ? 0 : 1];
}

bool AnyAttached ()
{
    return g_targets[0].canvas != nullptr || g_targets[1].canvas != nullptr;
}

// The attached canvas a message is for: its own window, or a child of it.
Target* TargetFor (HWND window)
{
    for (Target& target : g_targets)
        if (target.canvas != nullptr && (window == target.canvas || ::IsChild (target.canvas, window)))
            return &target;
    return nullptr;
}

uint32_t Held (WPARAM keys)
{
    uint32_t held = 0;
    if ((keys & MK_LBUTTON) != 0)
        held |= Bit (Button::Left);
    if ((keys & MK_RBUTTON) != 0)
        held |= Bit (Button::Right);
    if ((keys & MK_MBUTTON) != 0)
        held |= Bit (Button::Middle);
    if ((keys & MK_XBUTTON1) != 0)
        held |= Bit (Button::X1);
    if ((keys & MK_XBUTTON2) != 0)
        held |= Bit (Button::X2);
    return held;
}

// The message as an event; false for a mouse message the HUD has no use for.
bool EventOf (const MSG& message, Event& event)
{
    const WPARAM keys = GET_KEYSTATE_WPARAM (message.wParam);
    event.held = Held (keys);
    const Button x = GET_XBUTTON_WPARAM (message.wParam) == XBUTTON2 ? Button::X2 : Button::X1;
    switch (message.message) {
        case WM_MOUSEMOVE:
            event.kind = EventKind::Move;
            return true;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = Button::Left;
            return true;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = Button::Right;
            return true;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = Button::Middle;
            return true;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = x;
            return true;
        case WM_LBUTTONUP:
            event.kind = EventKind::Release;
            event.button = Button::Left;
            return true;
        case WM_RBUTTONUP:
            event.kind = EventKind::Release;
            event.button = Button::Right;
            return true;
        case WM_MBUTTONUP:
            event.kind = EventKind::Release;
            event.button = Button::Middle;
            return true;
        case WM_XBUTTONUP:
            event.kind = EventKind::Release;
            event.button = x;
            return true;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            event.kind = EventKind::Wheel;
            return true;
        default:
            return false;
    }
}

void Consider (MSG& message, bool removing)
{
    Target* const target = TargetFor (message.hwnd);
    if (target == nullptr)
        return;
    Event event;
    if (!EventOf (message, event))
        return;
    // MSG::pt is where the pointer was when the message was posted, in screen pixels;
    // the wheel's lParam is in screen pixels too, so every message is read from it.
    POINT point = message.pt;
    RECT client = {};
    if (!::ScreenToClient (target->canvas, &point) || !::GetClientRect (target->canvas, &client))
        return;
    const bool over = target->map.Hit (float (point.x), float (point.y), float (client.right - client.left),
                                       float (client.bottom - client.top)) >= 0;
    const bool shown = target->shown != nullptr && target->shown ();
    if (removing)
        ++g_stats.seen;
    if (!shown) {
        // Nothing of the HUD is on screen: every message is Archicad's, and the latch
        // lets go of a gesture the HUD can no longer finish.
        if (removing) {
            g_router.Reset ();
            if (over)
                ++g_stats.declinedHidden;
        }
        return;
    }
    const Route route = removing ? g_router.Decide (event, over) : g_router.Preview (event, over);
    if (route == Route::Take) {
        message.message = WM_NULL;
        if (removing) {
            ++g_stats.taken;
            if (event.kind == EventKind::Press)
                ++g_stats.takenPresses;
            else if (event.kind == EventKind::Move)
                ++g_stats.takenMoves;
        }
    }
    else if (over && removing) {
        ++g_stats.passedOverHud;
    }
}

LRESULT CALLBACK GetMessageProc (int code, WPARAM wParam, LPARAM lParam)
{
    // ⚠️ THE CHAIN IS CALLED ON EVERY PATH: skipping it breaks every other hook on this
    // thread, Archicad's own included.
    if (code == HC_ACTION && lParam != 0) {
        MSG* const message = reinterpret_cast<MSG*> (lParam);
        if (message->message >= WM_MOUSEFIRST && message->message <= WM_MOUSELAST)
            Consider (*message, (wParam & PM_REMOVE) != 0);
    }
    return ::CallNextHookEx (g_hook, code, wParam, lParam);
}

void Uninstall ()
{
    if (g_hook == nullptr)
        return;
    ::UnhookWindowsHookEx (g_hook);
    g_hook = nullptr;
    ArchVizLog ("OVERLAY INPUT  the HUD's message hook removed");
}

} // namespace

const char* ViewName (View view)
{
    return view == View::ThreeD ? "3D" : "plan";
}

bool Attach (View view, HWND canvas, ShownTest shown, std::string& error)
{
    Target& target = TargetOf (view);
    if (canvas == nullptr) {
        error = "no canvas to take the HUD's input from";
        return false;
    }
    const bool moved = target.canvas != canvas;
    target.canvas = canvas;
    target.shown = shown;
    if (g_hook == nullptr) {
        // Thread-local: our own thread's id, never 0.
        g_hook = ::SetWindowsHookExW (WH_GETMESSAGE, &GetMessageProc, nullptr, ::GetCurrentThreadId ());
        if (g_hook == nullptr) {
            error = "SetWindowsHookEx(WH_GETMESSAGE) failed with GetLastError " + std::to_string (::GetLastError ());
            target = Target {};
            return false;
        }
        ArchVizLog ("OVERLAY INPUT  the HUD's message hook installed on this thread");
    }
    if (moved) {
        g_router.Reset ();
        char line[160] = {};
        _snprintf_s (line, sizeof (line), _TRUNCATE, "OVERLAY INPUT  the %s HUD takes its input from canvas 0x%llx",
                     ViewName (view), (unsigned long long) (uintptr_t) canvas);
        ArchVizLog (line);
    }
    return true;
}

void Detach (View view)
{
    Target& target = TargetOf (view);
    const bool was = target.canvas != nullptr;
    target = Target {};
    g_router.Reset ();
    if (was) {
        ArchVizLog (std::string ("OVERLAY INPUT  the ") + ViewName (view) + " HUD takes no more input");
    }
    if (!AnyAttached ())
        Uninstall ();
}

void Shutdown ()
{
    for (Target& target : g_targets)
        target = Target {};
    g_router.Reset ();
    Uninstall ();
}

void SetHitMap (View view, HitMap map)
{
    TargetOf (view).map = std::move (map);
}

Stats GetStats ()
{
    Stats stats = g_stats;
    stats.installed = g_hook != nullptr;
    for (int i = 0; i < 2; ++i) {
        stats.attached[i] = g_targets[i].canvas != nullptr;
        stats.regions[i] = uint32_t (g_targets[i].map.regions.size ());
    }
    return stats;
}

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv
