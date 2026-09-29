// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. This hook runs inside Archicad's message dispatch: it decides and
// returns, it never calls ACAPI, and it is gone before its session is (§8, §11).
// ArchViz/OverlayInput -- see the header.

#include "ArchViz/OverlayInput.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <chrono>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlayinput {

namespace {

// The window the hook posts to; a view's refresh is WM_APP + 1 with the view in wParam,
// its paced redraw a timer of id 1 + view.
constexpr wchar_t kWindowClass[] = L"TapiocaOverlayInput";
constexpr UINT kRefreshMessage = WM_APP + 1;
constexpr UINT_PTR kRedrawTimer = 1;
// At most one redraw a view this often for the HUD: about thirty a second.
constexpr uint64_t kRedrawMs = 33;

struct Target {
    HWND canvas = nullptr;
    HudOwner owner;
    HitMap map;
    // The pointer, in the canvas's client pixels, and whether it is over the canvas.
    bool inside = false;
    float x = 0.0f;
    float y = 0.0f;
    bool wasOver = false; // over the HUD at the last message: leaving it redraws the hover
    std::vector<overlayhud::Input::Button> buttons;
    bool refreshPending = false;
    bool timerArmed = false;
    uint64_t lastRedrawMs = 0;
};

HHOOK g_hook = nullptr;
HWND g_window = nullptr;
ATOM g_windowClass = 0;
Target g_targets[2];
// One latch: the views never run together, and a gesture is one pointer's.
Router g_router;
Stats g_stats;

Target& TargetOf (View view)
{
    return g_targets[view == View::ThreeD ? 0 : 1];
}

View ViewOf (const Target& target)
{
    return &target == &g_targets[0] ? View::ThreeD : View::Plan;
}

bool AnyAttached ()
{
    return g_targets[0].canvas != nullptr || g_targets[1].canvas != nullptr;
}

uint32_t Since (std::chrono::steady_clock::time_point started)
{
    return uint32_t (
        std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - started).count ());
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

// Ask for the view's HUD to be laid out again, unless that is already asked. From the
// hook: a flag and a PostMessage.
void RequestRefresh (Target& target)
{
    if (g_window == nullptr || target.owner.refresh == nullptr || target.refreshPending)
        return;
    if (::PostMessageW (g_window, kRefreshMessage, WPARAM (ViewOf (target)), 0))
        target.refreshPending = true;
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
    const bool shown = target->owner.shown != nullptr && target->owner.shown ();
    if (removing)
        ++g_stats.seen;
    if (!shown) {
        // Nothing of the HUD is on screen: every message is Archicad's, and the latch
        // lets go of a gesture the HUD can no longer finish.
        if (removing) {
            g_router.Reset ();
            target->buttons.clear ();
            target->wasOver = false;
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
    if (!removing)
        return;
    // The pointer, for the HUD's next layout, and the buttons the HUD took.
    target->inside = PtInRect (&client, point) != FALSE;
    target->x = float (point.x);
    target->y = float (point.y);
    const bool button = route == Route::Take && (event.kind == EventKind::Press || event.kind == EventKind::Release);
    if (button && target->buttons.size () < 32)
        target->buttons.push_back ({ int (event.button), event.kind == EventKind::Press });
    // ⚠️ NOTHING WHILE ARCHICAD OWNS THE GESTURE: a wall drawn across a panel is not the
    // HUD's to redraw under.
    const bool hudsTurn = g_router.GetOwner () != Owner::Host;
    if (hudsTurn && (over || target->wasOver || button || g_router.GetOwner () == Owner::Hud))
        RequestRefresh (*target);
    target->wasOver = over;
}

// The pointer left the canvas: whatever it hovered is not hovered any more.
void Left (const MSG& message)
{
    Target* const target = TargetFor (message.hwnd);
    if (target == nullptr || !target->inside)
        return;
    target->inside = false;
    if (target->wasOver && g_router.GetOwner () != Owner::Host)
        RequestRefresh (*target);
    target->wasOver = false;
}

LRESULT CALLBACK GetMessageProc (int code, WPARAM wParam, LPARAM lParam)
{
    // ⚠️ THE CHAIN IS CALLED ON EVERY PATH: skipping it breaks every other hook on this
    // thread, Archicad's own included.
    if (code == HC_ACTION && lParam != 0) {
        MSG* const message = reinterpret_cast<MSG*> (lParam);
        if (message->message >= WM_MOUSEFIRST && message->message <= WM_MOUSELAST)
            Consider (*message, (wParam & PM_REMOVE) != 0);
        else if (message->message == WM_MOUSELEAVE && (wParam & PM_REMOVE) != 0)
            Left (*message);
    }
    return ::CallNextHookEx (g_hook, code, wParam, lParam);
}

void Redraw (Target& target)
{
    if (target.owner.redraw == nullptr)
        return;
    const auto started = std::chrono::steady_clock::now ();
    target.lastRedrawMs = ::GetTickCount64 ();
    target.owner.redraw ();
    const uint32_t took = Since (started);
    ++g_stats.redraws;
    g_stats.lastRedrawMicroseconds = took;
    g_stats.maxRedrawMicroseconds = (std::max) (g_stats.maxRedrawMicroseconds, took);
}

// A redraw now, or -- inside the interval since the last -- when it runs out.
void Pace (Target& target)
{
    const uint64_t now = ::GetTickCount64 ();
    const uint64_t since = now - target.lastRedrawMs;
    if (since >= kRedrawMs) {
        Redraw (target);
        return;
    }
    if (target.timerArmed)
        return;
    const UINT_PTR id = kRedrawTimer + UINT_PTR (ViewOf (target));
    if (::SetTimer (g_window, id, UINT (kRedrawMs - since), nullptr) != 0)
        target.timerArmed = true;
    else
        Redraw (target); // no timer: now rather than never
}

LRESULT CALLBACK WindowProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kRefreshMessage && wParam <= 1) {
        Target& target = g_targets[wParam];
        // ⚠️ CLEARED BEFORE THE LAYOUT: input arriving during it asks for another.
        target.refreshPending = false;
        if (target.canvas == nullptr || target.owner.refresh == nullptr)
            return 0;
        const auto started = std::chrono::steady_clock::now ();
        const bool changed = target.owner.refresh ();
        const uint32_t took = Since (started);
        ++g_stats.refreshes;
        g_stats.lastRefreshMicroseconds = took;
        g_stats.maxRefreshMicroseconds = (std::max) (g_stats.maxRefreshMicroseconds, took);
        if (changed) {
            ++g_stats.changes;
            Pace (target);
        }
        return 0;
    }
    if (message == WM_TIMER && wParam >= kRedrawTimer && wParam <= kRedrawTimer + 1) {
        ::KillTimer (window, wParam);
        Target& target = g_targets[wParam - kRedrawTimer];
        target.timerArmed = false;
        if (target.canvas != nullptr)
            Redraw (target);
        return 0;
    }
    return ::DefWindowProcW (window, message, wParam, lParam);
}

// ⚠️ HINSTANCE FROM THIS MODULE, as CameraWake's: a class registered against Archicad's
// outlives the add-on and the next registration fails after a reload.
bool CreateInputWindow (std::string& error)
{
    if (g_window != nullptr)
        return true;
    HMODULE module = nullptr;
    ::GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR> (&WindowProc), &module);
    if (g_windowClass == 0) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = module;
        wc.lpszClassName = kWindowClass;
        g_windowClass = ::RegisterClassExW (&wc);
        if (g_windowClass == 0 && ::GetLastError () != ERROR_CLASS_ALREADY_EXISTS) {
            error = "RegisterClassEx for the HUD's input window failed with GetLastError " +
                    std::to_string (::GetLastError ());
            return false;
        }
    }
    // HWND_MESSAGE: no screen presence, never enumerated; posted and sent messages only.
    g_window = ::CreateWindowExW (0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
    if (g_window == nullptr) {
        error = "CreateWindowEx(HWND_MESSAGE) for the HUD's input failed with GetLastError " +
                std::to_string (::GetLastError ());
        return false;
    }
    return true;
}

// The view's session is over: its timer, its pointer, its callbacks.
void Clear (Target& target)
{
    if (target.timerArmed && g_window != nullptr)
        ::KillTimer (g_window, kRedrawTimer + UINT_PTR (ViewOf (target)));
    target = Target {};
}

void Uninstall ()
{
    if (g_hook != nullptr) {
        ::UnhookWindowsHookEx (g_hook);
        g_hook = nullptr;
        ArchVizLog ("OVERLAY INPUT  the HUD's message hook removed");
    }
    // ⚠️ NOT GUARDED ON THE HOOK: the window can exist without it, and a window whose
    // procedure lives in a DLL about to unload is Windows calling into freed code.
    if (g_window != nullptr) {
        ::DestroyWindow (g_window);
        g_window = nullptr;
    }
}

overlayhud::Input InputOf (Target& target, bool take)
{
    overlayhud::Input input;
    RECT client = {};
    if (target.canvas != nullptr && ::GetClientRect (target.canvas, &client)) {
        input.width = float (client.right - client.left);
        input.height = float (client.bottom - client.top);
    }
    input.pointer = target.inside;
    input.x = target.x;
    input.y = target.y;
    if (take) {
        input.buttons = std::move (target.buttons);
        target.buttons.clear ();
    }
    return input;
}

} // namespace

const char* ViewName (View view)
{
    return view == View::ThreeD ? "3D" : "plan";
}

bool Attach (View view, HWND canvas, const HudOwner& owner, std::string& error)
{
    Target& target = TargetOf (view);
    if (canvas == nullptr) {
        error = "no canvas to take the HUD's input from";
        return false;
    }
    const bool moved = target.canvas != canvas;
    if (moved) {
        HitMap map = std::move (target.map); // where the HUD is does not move with the canvas
        Clear (target);
        target.map = std::move (map);
    }
    target.canvas = canvas;
    target.owner = owner;
    if (!CreateInputWindow (error)) {
        Clear (target);
        return false;
    }
    if (g_hook == nullptr) {
        // Thread-local: our own thread's id, never 0.
        g_hook = ::SetWindowsHookExW (WH_GETMESSAGE, &GetMessageProc, nullptr, ::GetCurrentThreadId ());
        if (g_hook == nullptr) {
            error = "SetWindowsHookEx(WH_GETMESSAGE) failed with GetLastError " + std::to_string (::GetLastError ());
            Clear (target);
            if (!AnyAttached ())
                Uninstall ();
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
    Clear (target);
    g_router.Reset ();
    if (was)
        ArchVizLog (std::string ("OVERLAY INPUT  the ") + ViewName (view) + " HUD takes no more input");
    if (!AnyAttached ())
        Uninstall ();
}

void Shutdown ()
{
    for (Target& target : g_targets)
        Clear (target);
    g_router.Reset ();
    Uninstall ();
}

void SetHitMap (View view, HitMap map)
{
    TargetOf (view).map = std::move (map);
}

overlayhud::Input TakeInput (View view)
{
    return InputOf (TargetOf (view), true);
}

overlayhud::Input CurrentInput (View view)
{
    return InputOf (TargetOf (view), false);
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
