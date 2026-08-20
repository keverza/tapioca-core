// ArchViz/CameraWake -- see the header. The hook body is the whole file, and
// every rule about it is in that header's comments.

#include "ArchViz/CameraWake.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "ArchViz/DiligentViewport.hpp"

#include <windows.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace camerawake {

namespace {

HHOOK g_hook = nullptr;

// ⚠️ ATOMICS, NOT A MUTEX. The hook runs inside Archicad's message dispatch and
// the readers run on the same thread from a timer, so a lock would be
// uncontended -- but a lock inside a message hook is exactly the shape that
// deadlocks the day something re-enters it, and there is nothing here that needs
// one. Every field is written by the hook and read by the timer.
std::atomic<uint64_t> g_lastInputMs {0};
std::atomic<uint64_t> g_wheelEvents {0};
std::atomic<uint64_t> g_dragEvents {0};
std::atomic<uint64_t> g_keyEvents {0};
std::atomic<uint64_t> g_pollsPosted {0};
std::atomic<uint64_t> g_pollsCoalesced {0};

// ⚠️ BLANKING IS OPT-IN, AND `wake` MUST NOT OPT IN. The hook is shared by two
// modes that want opposite things from it: `hideonnav` wants the overlay hidden
// during motion, `wake` wants it drawn as accurately as possible DURING motion.
// Blanking unconditionally made `wake` unusable on its first run -- the overlay
// simply vanished and stayed gone, because only `hideonnav`'s tick knows how to
// lift the blank again. Nothing was visible to judge, and the mode looked broken
// rather than mis-wired.
std::atomic<bool> g_blankOnInput {false};

// ---- the input-driven read ------------------------------------------------
// A message-only window: it never becomes visible, is never enumerated, and
// costs nothing but a queue entry. It exists so the hook has somewhere to POST
// to -- the hook itself must not read ACAPI (it runs re-entrant inside
// Archicad's dispatch), and a posted message is dispatched on the main thread at
// ordinary priority, ahead of WM_TIMER.
constexpr wchar_t kWindowClass[] = L"TapiocaCameraWake";
constexpr UINT    kPollMessage = WM_APP + 1;

HWND         g_window = nullptr;
ATOM         g_windowClass = 0;
PollCallback g_pollCallback = nullptr;

// Set while a poll is posted and not yet run. See the header: coalesced, never
// queued.
std::atomic<bool> g_pollPending {false};

LRESULT CALLBACK WakeWindowProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kPollMessage) {
        // ⚠️ CLEARED BEFORE THE READ, NOT AFTER. Clearing afterwards would
        // swallow every input arriving DURING the read -- which, at a read cost
        // of several milliseconds mid-drag, is most of them. The cost of
        // clearing first is at worst one redundant poll.
        g_pollPending.store (false, std::memory_order_release);
        if (g_pollCallback != nullptr)
            g_pollCallback ();
        return 0;
    }
    return DefWindowProcW (window, message, wParam, lParam);
}

// Ask for a poll unless one is already outstanding. Called FROM THE HOOK, so it
// does nothing but an atomic exchange and a PostMessage.
void RequestPoll ()
{
    if (g_window == nullptr || g_pollCallback == nullptr)
        return;
    if (g_pollPending.exchange (true, std::memory_order_acq_rel)) {
        g_pollsCoalesced.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    if (PostMessageW (g_window, kPollMessage, 0, 0))
        g_pollsPosted.fetch_add (1, std::memory_order_relaxed);
    else
        g_pollPending.store (false, std::memory_order_release);   // never strand the flag
}

// Is `message` the user navigating a view?
//
// ⚠️ MIDDLE BUTTON AND WHEEL, NOT EVERY MOUSE MOVE. Archicad pans on the wheel
// button and zooms on the wheel; a bare WM_MOUSEMOVE is the cursor crossing the
// screen, which is not navigation and would blank the overlay every time the
// user reached for a palette. The move only counts while a button that pans is
// held.
//
// ⚠️ THE MESSAGE IS NOT FILTERED BY WINDOW. The hook is already thread-local, so
// every message it sees belongs to Archicad's UI thread, and deciding WHICH of
// Archicad's windows counts as a view would mean hard-coding a window class this
// repo has already been wrong about twice. Over-triggering costs a blank frame;
// under-triggering costs the feature.
bool IsNavigationInput (UINT message, WPARAM wParam)
{
    switch (message) {
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            g_wheelEvents.fetch_add (1, std::memory_order_relaxed);
            return true;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            g_dragEvents.fetch_add (1, std::memory_order_relaxed);
            return true;
        case WM_MOUSEMOVE:
            // ⚠️ THE LEFT BUTTON COUNTS TOO, AND LEAVING IT OUT IS WHY THE FIRST
            // RUN REPORTED "does not always vanish". Archicad pans on the wheel
            // button, but it ALSO pans with the left button on the Pan tool, on a
            // scrollbar, and while space is held -- all of which are a left drag.
            // Missing them made the blank look intermittent, which reads as an
            // unreliable feature rather than an incomplete message filter.
            //
            // The cost of including it is a blanked frame whenever the user drags
            // anything at all, including a selection marquee. That is the right
            // trade for a mode whose entire purpose is not drawing during motion:
            // over-triggering costs one transparent frame, under-triggering costs
            // the feature.
            if ((wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) != 0) {
                g_dragEvents.fetch_add (1, std::memory_order_relaxed);
                return true;
            }
            return false;
        // Keyboard navigation: the arrow keys scroll a plan, and page keys jump
        // it. Cheap to include and invisible when unused.
        case WM_KEYDOWN:
        case WM_KEYUP:
            switch (wParam) {
                case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
                    g_keyEvents.fetch_add (1, std::memory_order_relaxed);
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

LRESULT CALLBACK GetMessageHook (int code, WPARAM wParam, LPARAM lParam)
{
    // ⚠️ THE CHAIN IS CALLED ON EVERY PATH, INCLUDING THE EARLY ONES. Skipping
    // CallNextHookEx breaks every other hook on this thread -- including
    // Archicad's own, if it has any -- and the symptom is input silently
    // vanishing somewhere unrelated.
    if (code == HC_ACTION && lParam != 0) {
        // ⚠️ PM_NOREMOVE MEANS THE MESSAGE IS BEING PEEKED, NOT CONSUMED, and it
        // will come back again. Counting it would multiply every wheel notch by
        // however many times the pump peeks; the timestamp would still be right,
        // but the stats a probe reports would be fiction.
        if (wParam != PM_NOREMOVE) {
            const MSG* message = reinterpret_cast<const MSG*> (lParam);
            if (IsNavigationInput (message->message, message->wParam)) {
                g_lastInputMs.store (GetTickCount64 (), std::memory_order_release);
                // ⚠️ THE BLANK IS SET FROM HERE, AND THAT IS THE ENTIRE POINT OF
                // THE HOOK. It is one atomic store into the render thread's
                // state -- no ACAPI, no allocation, no lock -- and it happens
                // BEFORE Archicad has processed the input that will move the
                // view. Setting it from the timer instead is what produced "the
                // overlay jumps and then disappears": by then the wrong frame
                // was already on screen.
                //
                // Only the blank is set here. Lifting it needs to know the view
                // has SETTLED, which is a timed decision and belongs on the
                // timer (ApplyHideOnNavigation).
                if (g_blankOnInput.load (std::memory_order_acquire))
                    DiligentViewport::Get ().SetBlanked (true);
                // And ask for a camera read at input priority. No-op unless a
                // callback was set, which is what keeps `hideonnav` exactly as
                // it was measured.
                RequestPoll ();
            }
        }
    }
    return CallNextHookEx (g_hook, code, wParam, lParam);
}

}   // namespace

void SetPollCallback (PollCallback callback)
{
    g_pollCallback = callback;
}

void SetBlankOnInput (bool blank)
{
    g_blankOnInput.store (blank, std::memory_order_release);
}

namespace {

// The message-only window, created lazily on Install and destroyed on Remove.
// ⚠️ HINSTANCE COMES FROM THIS MODULE, not from Archicad's. A class registered
// against the wrong instance outlives the add-on and the next RegisterClass
// fails with ERROR_CLASS_ALREADY_EXISTS after a reload -- the same shape of bug
// as a surviving hook, and just as invisible until the second load.
bool CreateWakeWindow (std::string& error)
{
    if (g_window != nullptr)
        return true;

    HMODULE module = nullptr;
    GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR> (&WakeWindowProc), &module);

    if (g_windowClass == 0) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = &WakeWindowProc;
        wc.hInstance = module;
        wc.lpszClassName = kWindowClass;
        g_windowClass = RegisterClassExW (&wc);
        if (g_windowClass == 0 && GetLastError () != ERROR_CLASS_ALREADY_EXISTS) {
            error = "RegisterClassEx for the camera wake window failed with GetLastError " +
                    std::to_string (GetLastError ());
            return false;
        }
    }

    // HWND_MESSAGE: no screen presence, no z-order, not enumerated. It can only
    // receive posted and sent messages, which is all this needs.
    g_window = CreateWindowExW (0, kWindowClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, module, nullptr);
    if (g_window == nullptr) {
        error = "CreateWindowEx(HWND_MESSAGE) for the camera wake window failed with "
                "GetLastError " + std::to_string (GetLastError ());
        return false;
    }
    return true;
}

void DestroyWakeWindow ()
{
    if (g_window != nullptr) {
        DestroyWindow (g_window);
        g_window = nullptr;
    }
    // ⚠️ THE PENDING FLAG IS CLEARED WITH THE WINDOW. A poll posted to a window
    // that is now gone will never run, so leaving the flag set would make the
    // NEXT arm refuse to post anything at all -- a wake path that silently never
    // fires, which looks identical to a hook that was never installed.
    g_pollPending.store (false, std::memory_order_release);
}

}   // namespace

bool Install (std::string& error)
{
    if (g_hook != nullptr)
        return true;

    if (!CreateWakeWindow (error)) {
        ArchVizLog ("camera wake: " + error);
        return false;
    }

    // Thread-local: the last argument is OUR thread id, never 0. See the header.
    g_hook = SetWindowsHookExW (WH_GETMESSAGE, &GetMessageHook, nullptr, GetCurrentThreadId ());
    if (g_hook == nullptr) {
        DestroyWakeWindow ();
        error = "SetWindowsHookEx(WH_GETMESSAGE) failed with GetLastError " +
                std::to_string (GetLastError ());
        ArchVizLog ("camera wake: " + error);
        return false;
    }
    g_lastInputMs.store (0, std::memory_order_release);
    ArchVizLog ("camera wake: WH_GETMESSAGE installed on this thread; navigation input is now "
                "seen before Archicad acts on it");
    return true;
}

void Remove ()
{
    // ⚠️ NOT GUARDED ON g_hook. The window can exist without the hook (Install
    // creates it first and unwinds on failure), and an early return here would
    // leave a live HWND whose WndProc lives in a DLL that is about to unload --
    // Windows then calls into freed code, which is the crash this whole file is
    // careful about.
    if (g_hook != nullptr) {
        UnhookWindowsHookEx (g_hook);
        g_hook = nullptr;
        ArchVizLog ("camera wake: hook removed");
    }
    DestroyWakeWindow ();
    g_pollCallback = nullptr;
    g_blankOnInput.store (false, std::memory_order_release);
}

bool Installed ()
{
    return g_hook != nullptr;
}

uint64_t MillisecondsSinceInput ()
{
    const uint64_t last = g_lastInputMs.load (std::memory_order_acquire);
    if (last == 0)
        return ~uint64_t (0) / 2;   // "never", and safe to compare against
    const uint64_t now = GetTickCount64 ();
    return now > last ? now - last : 0;
}

bool Navigating (uint32_t settleMs)
{
    return g_hook != nullptr && MillisecondsSinceInput () < settleMs;
}

Stats GetStats ()
{
    Stats stats;
    stats.installed = g_hook != nullptr;
    stats.wheelEvents = g_wheelEvents.load (std::memory_order_relaxed);
    stats.dragEvents = g_dragEvents.load (std::memory_order_relaxed);
    stats.keyEvents = g_keyEvents.load (std::memory_order_relaxed);
    stats.pollsPosted = g_pollsPosted.load (std::memory_order_relaxed);
    stats.pollsCoalesced = g_pollsCoalesced.load (std::memory_order_relaxed);
    return stats;
}

}   // namespace camerawake
}   // namespace archviz
}   // namespace geomsrv
