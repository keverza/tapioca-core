// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. This hook runs inside Archicad's message dispatch: it decides and
// returns, it never calls ACAPI, and it is gone before its session is (§8, §11).
// ArchViz/OverlayInput -- see the header.

#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayInputEvents.hpp"
#include "ArchViz/OverlayInputKeyboard.hpp"
#include "ArchViz/OverlayInputState.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp" // the Debug tab's console: what the user checks when something fails

#include <chrono>
#include <cwchar>
#include <cwctype>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlayinput {

// The state the cursor shares (OverlayInputState.hpp): one latch, since the views never run
// together and a gesture is one pointer's; the counters; the two views' targets.
namespace state {
Target targets[2];
Router router;
Stats stats;

View ViewOf (const Target& target)
{
    return &target == &targets[0] ? View::ThreeD : View::Plan;
}

// The attached canvas a message is for: its own window, or a child of it.
Target* TargetFor (HWND window)
{
    for (Target& target : targets)
        if (target.canvas != nullptr && (window == target.canvas || ::IsChild (target.canvas, window)))
            return &target;
    return nullptr;
}
} // namespace state

namespace {
using events::EventOf;
using events::PhysicalButtons;
using state::ReassertCursor;
using state::ShowCursor;
using state::Subclass;
using state::TargetFor;
using state::Unsubclass;
using state::ViewOf;

// The window the hook posts to; a view's refresh is WM_APP + 1 with the view in wParam,
// its paced redraw a timer of id 1 + view.
constexpr wchar_t kWindowClass[] = L"TapiocaOverlayInput";
constexpr UINT kRefreshMessage = WM_APP + 1;
constexpr UINT kCursorRepairMessage = WM_APP + 2;
constexpr UINT_PTR kRedrawTimer = 1;
// At most one redraw a view this often for the HUD: about sixty a second.
constexpr uint64_t kRedrawMs = 16; // hover feedback follows the pointer at up to 60 Hz

HHOOK g_hook = nullptr;
HHOOK s_returnHook = nullptr;
bool s_repairingCursor = false;
bool s_cursorRepairPending = false;
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
// The click meter: armed on demand. The idle hook, installed with the message hook, serves it
// and the cursor's repair.
bool g_timing = false;
HHOOK g_idleHook = nullptr;
overlayclicks::Meter g_meter;
ATOM g_windowClass = 0;
using state::Target;
Target (&g_targets)[2] = state::targets;
Target* s_gestureTarget = nullptr; // observe the owning canvas's release even on another window
Target* s_keyboardTarget = nullptr;
Router& g_router = state::router;
Stats& g_stats = state::stats;
bool s_roofEditKeys = false;
bool s_hudTextKeys = false;

Target& TargetOf (View view)
{
    return g_targets[view == View::ThreeD ? 0 : 1];
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
    const int hit = self->map.Hit (float (point.x), float (point.y), float (client.right - client.left),
                                   float (client.bottom - client.top));
    // Locked, the whole view is the HUD's: the router still leaves the wheel and the middle button
    // to Archicad, to navigate.
    const bool over = self->map.Owns (float (point.x), float (point.y), float (client.right - client.left),
                                      float (client.bottom - client.top));
    const bool overScroll = hit >= 0 && self->map.regions[size_t (hit)].scrolls;
    const bool shown = self->owner.shown != nullptr && self->owner.shown ();
    if (removing)
        ++g_stats.seen;
    if (!shown) {
        // Nothing of the HUD is on screen: every message is Archicad's, and the latch
        // lets go of a gesture the HUD can no longer finish.
        if (removing) {
            g_router.Reset ();
            s_gestureTarget = nullptr;
            self->buttons.clear ();
            self->wheel = 0.0f;
            self->wasOver = false;
            self->navigating = false;
            if (over)
                ++g_stats.declinedHidden;
        }
        return Route::Pass;
    }
    const Route route =
        removing ? g_router.Decide (event, over, overScroll) : g_router.Preview (event, over, overScroll);
    if (removing && event.kind == EventKind::Press && route == Route::Take) {
        s_gestureTarget = self, s_keyboardTarget = self;
        if (s_hudTextKeys && ::GetAncestor (self->canvas, GA_ROOT) == ::GetForegroundWindow ())
            ::SetFocus (self->canvas);
    }
    if (removing && g_router.GetOwner () == Owner::None)
        s_gestureTarget = nullptr;
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
    const bool middle =
        (event.kind == EventKind::Press || event.kind == EventKind::Release) && event.button == Button::Middle;
    if (route == Route::Pass && (middle || (event.kind == EventKind::Wheel && event.wheel != 0.0f))) {
        ++self->navigationRevision;
        if (middle)
            self->navigating = event.kind == EventKind::Press;
    }
    if (event.kind == EventKind::Move)
        self->navigating = (event.held & Bit (Button::Middle)) != 0;
    const bool button = route == Route::Take && (event.kind == EventKind::Press || event.kind == EventKind::Release);
    if (button && self->buttons.size () < 32)
        self->buttons.push_back ({ int (event.button), event.kind == EventKind::Press, true, self->x, self->y });
    // The wheel the HUD took: its page scrolls by it at the next layout.
    if (route == Route::Take && event.kind == EventKind::Wheel)
        self->wheel += event.wheel;
    // ⚠️ NOTHING WHILE ARCHICAD OWNS THE GESTURE: a wall drawn across a panel is not the
    // HUD's to redraw under.
    const bool hudsTurn = g_router.GetOwner () != Owner::Host;
    const bool hovering = self->owner.hovering != nullptr && self->owner.hovering ();
    if (hudsTurn && (over || self->wasOver || button || g_router.GetOwner () == Owner::Hud || hovering))
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
    if (Weigh (*target, event, message.pt, removing) == Route::Take) {
        message.message = WM_NULL;
        if (removing && event.kind == EventKind::Move && target->subclassed)
            ShowCursor (*target); // Archicad's pre-dispatch cursor must not flash through a locked hover
    }
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

// ⚠️ A PRESS THE HUD EATS STILL ACTIVATES ARCHICAD (the user, 2026-10-15: on the locked view a
// click did not bring Archicad in front of other applications, and keys then went elsewhere).
// Windows activates a window and focuses it as part of the click it delivers; an eaten click
// delivers none, so the HUD does what it would have: the canvas's top-level window comes to the
// front and the canvas takes the keyboard. This thread received the input, so the foreground
// change is allowed.
void Activate (const Target& target)
{
    if (target.canvas == nullptr)
        return;
    const HWND root = ::GetAncestor (target.canvas, GA_ROOT);
    if (root != nullptr && ::GetForegroundWindow () != root && ::SetForegroundWindow (root))
        ++g_stats.activations;
    const HWND focus = ::GetFocus ();
    if (focus != target.canvas && (focus == nullptr || !::IsChild (target.canvas, focus)))
        ::SetFocus (target.canvas);
}

// ⚠️ THE CHAIN IS CALLED ON EVERY PATH BUT ONE: a button message the HUD takes is eaten --
// the hook returns nonzero and Windows discards it, as the WH_MOUSE contract allows.
LRESULT CALLBACK MouseProc (int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam != 0 && wParam == WM_MOUSEMOVE) {
        const auto* info = reinterpret_cast<const MOUSEHOOKSTRUCT*> (lParam);
        Target* target = TargetFor (info->hwnd);
        Event move;
        move.kind = EventKind::Move;
        move.held = PhysicalButtons ();
        if (target != nullptr && target->map.locked && (move.held & Bit (Button::Middle)) == 0 &&
            Weigh (*target, move, info->pt, false) == Route::Take) {
            Weigh (*target, move, info->pt, true);
            ShowCursor (*target);
            return 1; // stop host hover feedback before its sent notifications, not after dispatch
        }
    }
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
        const bool outsideRelease =
            event.kind == EventKind::Release && s_gestureTarget != nullptr && target != s_gestureTarget;
        if (outsideRelease)
            Weigh (*s_gestureTarget, event, info->pt, true); // observe only; never swallow another window's event
        const bool taken =
            !outsideRelease && target != nullptr && Weigh (*target, event, info->pt, true) == Route::Take;
        if (g_timing && (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK))
            TimePress (info->hwnd, taken);
        if (taken) {
            ++g_stats.buttonsEaten;
            g_passed.valid = false;
            if (event.kind == EventKind::Press)
                Activate (*target);
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
    const bool hovering = target->owner.hovering != nullptr && target->owner.hovering ();
    if ((target->wasOver || hovering) && g_router.GetOwner () != Owner::Host)
        RequestRefresh (*target);
    target->wasOver = false;
}

// A window that takes typed text: an edit control, plain or rich, by its class.
bool TextField (HWND window)
{
    wchar_t name[64] = {};
    if (window == nullptr || ::GetClassNameW (window, name, 64) == 0)
        return false;
    for (wchar_t* c = name; *c; ++c)
        *c = wchar_t (::towlower (*c));
    return std::wcsstr (name, L"edit") != nullptr;
}

bool QueueKey (HWND window, const overlayhud::Input::Key& key, const std::u16string& text, bool take)
{
    if ((!s_roofEditKeys && !s_hudTextKeys) || !window)
        return false;
    if (s_hudTextKeys && !s_roofEditKeys) {
        Target* target = s_keyboardTarget;
        if (!target || !target->canvas || TextField (window) || !state::OwnsPointer (*target))
            return false;
        const HWND root = ::GetAncestor (target->canvas, GA_ROOT);
        if (root != ::GetForegroundWindow () || ::GetAncestor (window, GA_ROOTOWNER) != root)
            return false;
        if (take) {
            if (key.code && target->keys.size () < 128)
                target->keys.push_back (key);
            if (target->characters.size () + text.size () < 256)
                target->characters += text;
            RequestRefresh (*target);
        }
        return true;
    }
    Target* target = TargetFor (window);
    // Taken mouse presses do not focus Archicad's canvas. The document/frame may
    // retain keyboard focus and translate Delete as an accelerator there.
    if (!target) {
        if (s_keyboardTarget && ::IsChild (window, s_keyboardTarget->canvas))
            target = s_keyboardTarget;
        else
            for (auto& candidate : g_targets)
                if (candidate.canvas && ::IsChild (window, candidate.canvas) && candidate.map.locked &&
                    candidate.owner.shown && candidate.owner.shown ()) {
                    target = &candidate;
                    break;
                }
    }
    // ⚠️ AND OVER THE LOCKED VIEW, WHEREVER THE FOCUS IS (the user, 2026-10-15: keys sometimes went
    // to Archicad -- the focus stays in the palette that started the editor). The pointer on a
    // locked view the HUD owns makes its keys the HUD's, unless the focus is a text field being
    // typed into.
    if (!target && !TextField (window))
        for (auto& candidate : g_targets)
            if (candidate.canvas && candidate.map.locked && state::OwnsPointer (candidate)) {
                target = &candidate;
                ++g_stats.keysByPointer;
                break;
            }
    if (!target || !target->map.locked || !target->owner.shown || !target->owner.shown ())
        return false; // unrelated palettes, modal dialogs and unlocked canvases pass
    if (take) {
        if (key.code && target->keys.size () < 128)
            target->keys.push_back (key);
        if (target->characters.size () + text.size () < 256)
            target->characters += text;
        RequestRefresh (*target);
    }
    return true;
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
        // ⚠️ AND AT EVERY MESSAGE THE THREAD TAKES (the user, 2026-10-15: the cursor still glitched
        // through drags). Archicad also sets its cursor handling messages that are not the canvas's
        // -- its timers, its idle -- which the canvas subclass never sees; whatever the previous
        // message set is undone before the next one runs.
        if (removing)
            for (const Target& target : g_targets)
                if (target.subclassed)
                    ReassertCursor (target);
        // WH_CALLWNDPROCRET covers sent messages, not DispatchMessage's posted messages.
        // Coalesce one cursor-only turn after this dispatch, even if the pointer stops.
        if (removing && (original != kCursorRepairMessage || window != g_window) && !s_cursorRepairPending &&
            g_window != nullptr)
            for (const Target& target : g_targets)
                if (target.subclassed && target.map.locked && state::OwnsPointer (target)) {
                    s_cursorRepairPending = ::PostMessageW (g_window, kCursorRepairMessage, 0, 0) != 0;
                    break;
                }
        if (original >= WM_MOUSEFIRST && original <= WM_MOUSELAST)
            Consider (*message, removing);
        else if (original == WM_MOUSEHOVER) {
            const Target* target = TargetFor (window);
            if (target && target->map.locked && target->owner.shown && target->owner.shown ())
                message->message = WM_NULL;
        }
        else if (original == WM_MOUSELEAVE && removing)
            Left (*message);
        else if (s_roofEditKeys || s_hudTextKeys)
            keyboard::Message (*message, removing);
        // The click meter's press, when no mouse hook timed it first.
        if (g_timing && removing && g_mouseHook == nullptr &&
            (original == WM_LBUTTONDOWN || original == WM_LBUTTONDBLCLK))
            TimePress (window, message->message == WM_NULL);
    }
    return ::CallNextHookEx (g_hook, code, wParam, lParam);
}

// The thread is about to go idle: the busy stretch the meter is timing ends.
// ⚠️ AND IT IS WHEN ARCHICAD SETS ITS CURSOR MOST (the user, 2026-10-15: the cursor still glitched,
// and the log counted 2098 repairs in a session against about one a drag). Its idle processing
// runs after the queue empties, so the repair at the next message came only when the pointer next
// moved; until then Archicad's cursor showed. The thread about to wait is the last moment before
// the screen settles: the HUD's cursor goes back there, counted with the rest.
LRESULT CALLBACK IdleProc (int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        if (g_timing)
            g_meter.Idle (Micros ());
        for (const Target& target : g_targets)
            if (target.subclassed)
                ReassertCursor (target);
    }
    return ::CallNextHookEx (g_idleHook, code, wParam, lParam);
}

// OVERLAY-INVARIANTS.md §15: another native window can replace the cursor INSIDE dispatch.
// A pre-dispatch repair cannot win that write. This hook is local to our input thread.
LRESULT CALLBACK ReturnProc (int code, WPARAM wParam, LPARAM lParam)
{
    const LRESULT result = ::CallNextHookEx (s_returnHook, code, wParam, lParam);
    if (code == HC_ACTION && !s_repairingCursor) {
        s_repairingCursor = true;
        for (const Target& target : g_targets)
            if (target.subclassed)
                ReassertCursor (target);
        s_repairingCursor = false;
    }
    return result;
}

void FollowReturnHook ()
{
    if (g_hook != nullptr && s_returnHook == nullptr) {
        s_returnHook = ::SetWindowsHookExW (WH_CALLWNDPROCRET, &ReturnProc, nullptr, ::GetCurrentThreadId ());
        if (s_returnHook == nullptr)
            ArchVizLog ("OVERLAY INPUT  SetWindowsHookEx(WH_CALLWNDPROCRET) failed with GetLastError " +
                        std::to_string (::GetLastError ()) + ": native dispatch may replace the HUD cursor");
    }
    else if (g_hook == nullptr && s_returnHook != nullptr) {
        ::UnhookWindowsHookEx (s_returnHook);
        s_returnHook = nullptr;
        s_repairingCursor = false;
    }
}

// With the message hook: the cursor's repair needs it always, the click meter while armed.
void FollowIdleHook ()
{
    const bool wanted = g_hook != nullptr;
    if (wanted && g_idleHook == nullptr) {
        g_idleHook = ::SetWindowsHookExW (WH_FOREGROUNDIDLE, &IdleProc, nullptr, ::GetCurrentThreadId ());
        if (g_idleHook == nullptr)
            ArchVizLog ("OVERLAY INPUT  SetWindowsHookEx(WH_FOREGROUNDIDLE) failed with GetLastError " +
                        std::to_string (::GetLastError ()) +
                        ": clicks are not timed, and Archicad's idle cursor shows over the HUD");
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
    if (message == kCursorRepairMessage) {
        s_cursorRepairPending = false;
        for (const Target& target : g_targets)
            if (target.subclassed)
                ReassertCursor (target);
        return 0; // cursor only: no layout, geometry publication, redraw or navigation
    }
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
    if (s_gestureTarget == &target)
        s_gestureTarget = nullptr;
    if (s_keyboardTarget == &target)
        s_keyboardTarget = nullptr;
    Unsubclass (target);
    if (target.timerArmed && g_window != nullptr)
        ::KillTimer (g_window, kRedrawTimer + UINT_PTR (ViewOf (target)));
    target = Target {};
}

void Uninstall ()
{
    keyboard::Stop ();
    // Resolved procedures belong to comctl32, not to this session. Resolve again
    // on the next attach instead of retaining addresses across module teardown.
    state::ForgetSubclassing ();
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
    FollowReturnHook ();
    s_cursorRepairPending = false;
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
    // Coalesced layout messages can sit behind queued moves. Sample the current
    // cursor for hover, but preserve event coordinates when delivering a click.
    POINT screen = {};
    if (target.buttons.empty () && target.canvas != nullptr && ::GetCursorPos (&screen)) {
        const HWND under = ::WindowFromPoint (screen);
        POINT point = screen;
        if (::ScreenToClient (target.canvas, &point)) {
            input.pointer = (under == target.canvas || ::IsChild (target.canvas, under)) && point.x >= 0 &&
                            point.y >= 0 && point.x < input.width && point.y < input.height;
            input.x = float (point.x);
            input.y = float (point.y);
        }
    }
    // Locking routes canvas interactions to us, but does not turn the canvas into a panel.
    input.overHud = input.pointer && target.map.Hit (input.x, input.y, input.width, input.height) >= 0;
    input.hasButtonState = true;
    input.buttonsDown = PhysicalButtons ();
    input.navigating = target.navigating || (input.buttonsDown & Bit (Button::Middle)) != 0;
    input.navigationRevision = target.navigationRevision;
    input.viewIdentity = uint64_t (reinterpret_cast<uintptr_t> (target.canvas));
    // Read as the layout is: it follows the press that asked for it, the key held through both.
    input.shift = ::GetKeyState (VK_SHIFT) < 0;
    input.ctrl = ::GetKeyState (VK_CONTROL) < 0;
    if (take) {
        input.buttons = std::move (target.buttons);
        input.editActions = std::move (target.editActions);
        input.keys = std::move (target.keys);
        input.characters = std::move (target.characters);
        target.keys.clear (), target.characters.clear ();
        target.editActions.clear ();
        target.buttons.clear ();
        input.wheel = target.wheel;
        target.wheel = 0.0f;
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
        FollowReturnHook ();
    }
    if (g_mouseHook == nullptr) {
        g_mouseHook = ::SetWindowsHookExW (WH_MOUSE, &MouseProc, nullptr, ::GetCurrentThreadId ());
        g_passed = Passed ();
        if (g_mouseHook == nullptr) {
            const DWORD code = ::GetLastError ();
            ArchVizLog ("OVERLAY INPUT  SetWindowsHookEx(WH_MOUSE) failed with GetLastError " + std::to_string (code) +
                        ": the message hook decides the buttons, after Windows has told the canvas's parent");
            hudconsole::Warning ("HUD", "a click on the HUD may reach Archicad too: its mouse hook was refused (" +
                                            std::to_string (code) + ")");
        }
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
    if (s_roofEditKeys || s_hudTextKeys)
        keyboard::Install (&QueueKey);
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
    s_roofEditKeys = false;
    s_hudTextKeys = false;
    g_timing = false;
    for (Target& target : g_targets)
        Clear (target);
    g_router.Reset ();
    Uninstall ();
}

bool EnableRoofEditKeys (bool enabled)
{
    s_roofEditKeys = enabled;
    if (enabled) {
        if (!keyboard::Install (&QueueKey)) {
            s_roofEditKeys = false;
            hudconsole::Warning ("Roof input", "Keyboard ownership could not be installed; editing was refused.");
            return false;
        }
    }
    else if (!s_hudTextKeys)
        keyboard::Stop ();
    if (!enabled && !s_hudTextKeys)
        for (auto& target : g_targets)
            target.editActions.clear (), target.keys.clear (), target.characters.clear ();
    return true;
}

bool EnableHudTextKeys (bool enabled)
{
    const bool starting = enabled && !s_hudTextKeys;
    s_hudTextKeys = enabled;
    // A consumed HUD click does not normally move native focus. Focus THIS
    // canvas once on explicit text activation, never another window or a timer's
    // recurring refresh, so an old palette Edit cannot receive criteria text.
    if (starting && s_keyboardTarget && s_keyboardTarget->canvas && state::OwnsPointer (*s_keyboardTarget) &&
        ::GetAncestor (s_keyboardTarget->canvas, GA_ROOT) == ::GetForegroundWindow ())
        ::SetFocus (s_keyboardTarget->canvas);
    if (enabled && !keyboard::Install (&QueueKey)) {
        s_hudTextKeys = false;
        hudconsole::Warning ("Criteria input",
                             "Keyboard ownership could not be installed; use captured dropdown values.");
        return false;
    }
    if (!enabled && !s_roofEditKeys)
        keyboard::Stop ();
    return true;
}

void SetHitMap (View view, HitMap map)
{
    Target& target = TargetOf (view);
    const bool locking = map.locked && !target.map.locked;
    target.map = std::move (map);
    if (locking && target.canvas != nullptr && target.owner.shown != nullptr && target.owner.shown ()) {
        TRACKMOUSEEVENT tracking { sizeof (TRACKMOUSEEVENT), TME_CANCEL | TME_HOVER, target.canvas, 0 };
        ::TrackMouseEvent (&tracking);
        ::SendMessageW (target.canvas, WM_MOUSELEAVE, 0, 0); // clear feedback scheduled before the lock
    }
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
        stats.locked[i] = g_targets[i].map.locked;
    }
    stats.keyboardHook = keyboard::Installed ();
    stats.idleHook = g_idleHook != nullptr;
    stats.returnHook = s_returnHook != nullptr;
    stats.inputWindow = g_window != nullptr;
    stats.roofEditKeys = s_roofEditKeys;
    stats.hudTextKeys = s_hudTextKeys;
    stats.gestureHeld = s_gestureTarget != nullptr;
    stats.buttonsDown = g_down;
    return stats;
}

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv
