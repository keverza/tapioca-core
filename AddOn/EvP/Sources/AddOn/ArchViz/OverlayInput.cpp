// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. This hook runs inside Archicad's message dispatch: it decides and
// returns, it never calls ACAPI, and it is gone before its session is (§8, §11).
// ArchViz/OverlayInput -- see the header.

#include "ArchViz/OverlayInput.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <commctrl.h>

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
    // The canvas belongs to this thread (the hook sees its messages only then), and its
    // WM_SETCURSOR is answered by the subclass below.
    bool ownThread = false;
    bool subclassed = false;
};

HHOOK g_hook = nullptr;
// ⚠️ THE BUTTONS ARE DECIDED A STEP EARLIER, BY A THREAD-LOCAL WH_MOUSE HOOK (the user,
// 2026-09-30: a right click on the HUD still reached Archicad). Windows takes a button
// message off the queue in stages: the mouse hook first, then it SENDS the canvas's parent
// WM_PARENTNOTIFY and the canvas WM_MOUSEACTIVATE and WM_SETCURSOR, and only then hands the
// message to GetMessage -- where the message hook rewrites it, too late for what was sent.
// A message the mouse hook eats is none of those: nothing of Archicad's hears the click.
HHOOK g_mouseHook = nullptr;
// The last button message the mouse hook passed on: the message hook, seeing it next, does
// not decide it a second time.
struct Passed {
    bool valid = false;
    UINT message = 0;
    POINT pt = {};
} g_passed;
// The buttons down, as the button messages said: the mouse hook's are given no key state.
uint32_t g_down = 0;
HWND g_window = nullptr;
// The click meter: armed on demand, its idle hook installed with the message hook.
bool g_timing = false;
HHOOK g_idleHook = nullptr;
overlayclicks::Meter g_meter;
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

// The performance counter in microseconds: the click meter's clock.
uint64_t Micros ()
{
    static const int64_t frequency = [] () {
        LARGE_INTEGER f = {};
        ::QueryPerformanceFrequency (&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    LARGE_INTEGER now = {};
    ::QueryPerformanceCounter (&now);
    return uint64_t (now.QuadPart / frequency) * 1000000u +
           uint64_t (now.QuadPart % frequency) * 1000000u / uint64_t (frequency);
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

// The HUD owns what is under the pointer now: the canvas itself is there -- not a palette
// floating over it -- and a region of the last layout, or a gesture the HUD holds.
bool OwnsPointer (const Target& target)
{
    if (target.owner.shown == nullptr || !target.owner.shown ())
        return false;
    if (g_router.GetOwner () != Owner::None)
        return g_router.GetOwner () == Owner::Hud;
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
    return target.map.Hit (float (point.x), float (point.y), float (client.right - client.left),
                           float (client.bottom - client.top)) >= 0;
}

// ⚠️ AN ARROW OVER THE HUD, A HAND OVER WHAT IT CAN PRESS (the user, 2026-09-29) -- never
// the tool's cursor Archicad would show over the model there. False when the pointer is
// not the HUD's: Archicad's cursor stands.
bool ShowCursor (const Target& target)
{
    if (!OwnsPointer (target))
        return false;
    ::SetCursor (target.map.hand ? g_handCursor : g_arrowCursor);
    ++g_stats.cursorsSet;
    if (target.map.hand)
        ++g_stats.handsShown;
    return true;
}

LRESULT CALLBACK CanvasProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    if (message == WM_NCDESTROY) {
        // The canvas is going: the subclass goes with it.
        g_removeSubclass (window, &CanvasProc, id);
        for (Target& target : g_targets)
            if (target.canvas == window)
                target.subclassed = false;
        return g_defSubclass (window, message, wParam, lParam);
    }
    if (message == WM_SETCURSOR && LOWORD (lParam) == HTCLIENT) {
        const Target* const target = TargetFor (window);
        if (target != nullptr && ShowCursor (*target))
            return TRUE;
    }
    // ⚠️ AND NO CONTEXT MENU OF ARCHICAD'S OVER THE HUD, however it was asked for: a right
    // click there is the HUD's (its own menu). The keyboard's (lParam -1) is Archicad's.
    if (message == WM_CONTEXTMENU && lParam != LPARAM (-1)) {
        const Target* const target = TargetFor (window);
        if (target != nullptr && OwnsPointer (*target)) {
            ++g_stats.contextMenusSwallowed;
            return 0;
        }
    }
    return g_defSubclass (window, message, wParam, lParam);
}

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

// Ask for the view's HUD to be laid out again, unless that is already asked. From the
// hook: a flag and a PostMessage.
void RequestRefresh (Target& target)
{
    if (g_window == nullptr || target.owner.refresh == nullptr || target.refreshPending)
        return;
    if (::PostMessageW (g_window, kRefreshMessage, WPARAM (ViewOf (target)), 0))
        target.refreshPending = true;
}

// A mouse event on an attached canvas, at `screen`: its route, and -- removed from the
// queue, not only peeked at -- the latch moved, the pointer and the buttons the HUD took
// recorded for its next layout.
Route Weigh (Target& target, const Event& event, POINT screen, bool removing)
{
    POINT point = screen;
    RECT client = {};
    if (!::ScreenToClient (target.canvas, &point) || !::GetClientRect (target.canvas, &client))
        return Route::Pass;
    Target* const self = &target;
    const bool over = self->map.Hit (float (point.x), float (point.y), float (client.right - client.left),
                                     float (client.bottom - client.top)) >= 0;
    const bool shown = self->owner.shown != nullptr && self->owner.shown ();
    if (removing)
        ++g_stats.seen;
    if (!shown) {
        // Nothing of the HUD is on screen: every message is Archicad's, and the latch
        // lets go of a gesture the HUD can no longer finish.
        if (removing) {
            g_router.Reset ();
            self->buttons.clear ();
            self->wasOver = false;
            if (over)
                ++g_stats.declinedHidden;
        }
        return Route::Pass;
    }
    const Route route = removing ? g_router.Decide (event, over) : g_router.Preview (event, over);
    if (route == Route::Take) {
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
        return route;
    // The pointer, for the HUD's next layout, and the buttons the HUD took.
    self->inside = PtInRect (&client, point) != FALSE;
    self->x = float (point.x);
    self->y = float (point.y);
    const bool button = route == Route::Take && (event.kind == EventKind::Press || event.kind == EventKind::Release);
    if (button && self->buttons.size () < 32)
        self->buttons.push_back ({ int (event.button), event.kind == EventKind::Press });
    // ⚠️ NOTHING WHILE ARCHICAD OWNS THE GESTURE: a wall drawn across a panel is not the
    // HUD's to redraw under.
    const bool hudsTurn = g_router.GetOwner () != Owner::Host;
    if (hudsTurn && (over || self->wasOver || button || g_router.GetOwner () == Owner::Hud))
        RequestRefresh (*self);
    self->wasOver = over;
    return route;
}

bool IsButton (UINT message)
{
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            return true;
        default:
            return false;
    }
}

// A message the message hook sees: moves and the wheel are decided here; a button message
// only when the mouse hook did not decide it first (posted by a program, not the mouse; or
// no mouse hook), counted.
void Consider (MSG& message, bool removing)
{
    Target* const target = TargetFor (message.hwnd);
    if (target == nullptr)
        return;
    Event event;
    if (!EventOf (message, event))
        return;
    if (event.kind == EventKind::Move && removing)
        g_down = event.held;
    if (IsButton (message.message)) {
        const bool decided = g_passed.valid && g_passed.message == message.message && g_passed.pt.x == message.pt.x &&
                             g_passed.pt.y == message.pt.y;
        if (decided) {
            if (removing)
                g_passed.valid = false;
            return;
        }
        if (removing)
            ++g_stats.buttonsLate;
    }
    // MSG::pt is where the pointer was when the message was posted, in screen pixels;
    // the wheel's lParam is in screen pixels too, so every message is read from it.
    if (Weigh (*target, event, message.pt, removing) == Route::Take)
        message.message = WM_NULL;
}

// The click meter's press: on the HUD when the HUD took it, on the view when it passed it,
// on any other window of the thread otherwise.
void TimePress (HWND window, bool taken)
{
    const overlayclicks::Target target = TargetFor (window) == nullptr ? overlayclicks::Target::Other
                                         : taken                       ? overlayclicks::Target::Hud
                                                                       : overlayclicks::Target::View;
    char name[32] = {};
    if (window == nullptr || ::GetClassNameA (window, name, int (sizeof (name))) == 0)
        name[0] = '\0';
    g_meter.Press (target, name, Micros ());
}

// ⚠️ THE CHAIN IS CALLED ON EVERY PATH BUT ONE: a button message the HUD takes is eaten --
// the hook returns nonzero and Windows discards it, as the WH_MOUSE contract allows.
LRESULT CALLBACK MouseProc (int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam != 0 && IsButton (UINT (wParam))) {
        const MOUSEHOOKSTRUCTEX* const info = reinterpret_cast<const MOUSEHOOKSTRUCTEX*> (lParam);
        MSG message = {};
        message.hwnd = info->hwnd;
        message.message = UINT (wParam);
        message.pt = info->pt;
        // No key state comes with it: the buttons down as the messages said, this one
        // pressed or let go; the X button from the hook's own mouse data.
        message.wParam = MAKEWPARAM (0, HIWORD (info->mouseData));
        Event event;
        EventOf (message, event);
        const uint32_t bit = Bit (event.button);
        g_down = event.kind == EventKind::Press ? (g_down | bit) : (g_down & ~bit);
        event.held = g_down;
        Target* const target = TargetFor (info->hwnd);
        const bool taken = target != nullptr && Weigh (*target, event, info->pt, true) == Route::Take;
        if (g_timing && (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK))
            TimePress (info->hwnd, taken);
        if (taken) {
            ++g_stats.buttonsEaten;
            g_passed.valid = false;
            return 1;
        }
        g_passed.valid = true;
        g_passed.message = message.message;
        g_passed.pt = info->pt;
    }
    return ::CallNextHookEx (g_mouseHook, code, wParam, lParam);
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
        const bool removing = (wParam & PM_REMOVE) != 0;
        const UINT original = message->message;
        const HWND window = message->hwnd;
        if (g_timing && removing)
            g_meter.Wake (Micros ());
        if (original >= WM_MOUSEFIRST && original <= WM_MOUSELAST)
            Consider (*message, removing);
        else if (original == WM_MOUSELEAVE && removing)
            Left (*message);
        // The click meter's press, when no mouse hook timed it first.
        if (g_timing && removing && g_mouseHook == nullptr &&
            (original == WM_LBUTTONDOWN || original == WM_LBUTTONDBLCLK))
            TimePress (window, message->message == WM_NULL);
    }
    return ::CallNextHookEx (g_hook, code, wParam, lParam);
}

// The thread is about to go idle: the busy stretch the meter is timing ends.
LRESULT CALLBACK IdleProc (int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_timing)
        g_meter.Idle (Micros ());
    return ::CallNextHookEx (g_idleHook, code, wParam, lParam);
}

// With the message hook, while the meter is armed.
void FollowIdleHook ()
{
    const bool wanted = g_timing && g_hook != nullptr;
    if (wanted && g_idleHook == nullptr) {
        g_idleHook = ::SetWindowsHookExW (WH_FOREGROUNDIDLE, &IdleProc, nullptr, ::GetCurrentThreadId ());
        if (g_idleHook == nullptr)
            ArchVizLog ("OVERLAY INPUT  SetWindowsHookEx(WH_FOREGROUNDIDLE) failed with GetLastError " +
                        std::to_string (::GetLastError ()) + ": clicks are not timed");
    }
    else if (!wanted && g_idleHook != nullptr) {
        ::UnhookWindowsHookEx (g_idleHook);
        g_idleHook = nullptr;
    }
}

void Redraw (Target& target)
{
    if (target.owner.redraw == nullptr)
        return;
    const auto started = std::chrono::steady_clock::now ();
    target.lastRedrawMs = ::GetTickCount64 ();
    target.owner.redraw ();
    const uint32_t took = Since (started);
    if (g_timing)
        g_meter.Redraw (took, Micros ());
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
        if (g_timing)
            g_meter.Layout (took, Micros ());
        ++g_stats.refreshes;
        g_stats.lastRefreshMicroseconds = took;
        g_stats.maxRefreshMicroseconds = (std::max) (g_stats.maxRefreshMicroseconds, took);
        if (changed) {
            ++g_stats.changes;
            Pace (target);
        }
        // The hand the layout found, now: not at the pointer's next move.
        if (target.inside && target.subclassed)
            ShowCursor (target);
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
    Unsubclass (target);
    if (target.timerArmed && g_window != nullptr)
        ::KillTimer (g_window, kRedrawTimer + UINT_PTR (ViewOf (target)));
    target = Target {};
}

void Uninstall ()
{
    if (g_mouseHook != nullptr) {
        ::UnhookWindowsHookEx (g_mouseHook);
        g_mouseHook = nullptr;
    }
    g_passed = Passed ();
    g_down = 0;
    if (g_hook != nullptr) {
        ::UnhookWindowsHookEx (g_hook);
        g_hook = nullptr;
        ArchVizLog ("OVERLAY INPUT  the HUD's message hook removed");
    }
    FollowIdleHook ();
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
        FollowIdleHook ();
    }
    if (g_mouseHook == nullptr) {
        g_mouseHook = ::SetWindowsHookExW (WH_MOUSE, &MouseProc, nullptr, ::GetCurrentThreadId ());
        g_passed = Passed ();
        if (g_mouseHook == nullptr)
            ArchVizLog ("OVERLAY INPUT  SetWindowsHookEx(WH_MOUSE) failed with GetLastError " +
                        std::to_string (::GetLastError ()) +
                        ": the message hook decides the buttons, after Windows has told the canvas's parent");
    }
    if (moved) {
        g_router.Reset ();
        Subclass (target, view);
        char line[200] = {};
        _snprintf_s (line, sizeof (line), _TRUNCATE, "OVERLAY INPUT  the %s HUD takes its input from canvas 0x%llx%s",
                     ViewName (view), (unsigned long long) (uintptr_t) canvas,
                     target.subclassed ? ", and shows its own cursor over itself" : "");
        ArchVizLog (line);
        // ⚠️ LAID OUT AGAIN FOR THE CANVAS IT NOW TAKES (the user, 2026-09-30: after 3D ->
        // plan -> 3D, and after the 3D window closed and reopened, the HUD was drawn but
        // static, and took nothing). Detach forgets where the HUD is; until a layout says
        // it again every message passes to Archicad -- and only a message the HUD took
        // asked for a layout. The first session worked only because a layer set before it
        // attached had published the regions.
        RequestRefresh (target);
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
    g_timing = false;
    for (Target& target : g_targets)
        Clear (target);
    g_router.Reset ();
    Uninstall ();
}

void SetHitMap (View view, HitMap map)
{
    TargetOf (view).map = std::move (map);
}

void RequestLayout (View view)
{
    Target& target = TargetOf (view);
    if (target.canvas != nullptr)
        RequestRefresh (target);
}

void TimeClicks (bool on)
{
    g_timing = on;
    g_meter.Reset ();
    FollowIdleHook ();
    ArchVizLog (on ? "OVERLAY INPUT  clicks timed: the main thread's busy stretches for half a second after each press"
                   : "OVERLAY INPUT  clicks no longer timed");
}

bool TimingClicks ()
{
    return g_timing && g_idleHook != nullptr;
}

std::vector<overlayclicks::Sample> ClickSamples ()
{
    return g_meter.Samples (Micros ());
}

uint64_t ClickIdles ()
{
    return g_meter.Idles ();
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
    stats.mouseHook = g_mouseHook != nullptr;
    for (int i = 0; i < 2; ++i) {
        stats.attached[i] = g_targets[i].canvas != nullptr;
        stats.regions[i] = uint32_t (g_targets[i].map.regions.size ());
        stats.canvasOnThread[i] = g_targets[i].ownThread;
        stats.subclassed[i] = g_targets[i].subclassed;
    }
    return stats;
}

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv
