// ⚠️ BOUND BY OVERLAY-INVARIANTS.md (§7, §8, §15, §16) -- like the input hook it serves, this
// runs inside Archicad's message dispatch: it decides and returns, it never calls ACAPI.
// ArchViz/OverlayInputHostMode -- the pointer is Archicad's although the view is locked.
//
// ⚠️ A LOCKED VIEW IS THE HUD'S ONLY WHILE ARCHICAD IS NOT IN A POINTER MODE OF ITS OWN (the
// user, 2026-11-14, #145: the Orbit tool did nothing on the locked view and worked on an unlocked
// one; in Explore the pointer is invisible, held at the middle of the view, and the virtual
// editor's objects lit up under it). Locked, a left press anywhere is the HUD's (OverlayHitMap.hpp)
// -- but Orbit, Pan and Zoom drag with that button, and Explore reads every move, click and key.
// Archicad's API names none of these modes (the DevKit was searched), so each is read from what it
// does to the cursor:
//   - Explore HIDES the cursor: `GetCursorInfo` reports it not showing.
//   - Orbit, Pan, Zoom and Look show one of Archicad's own navigation cursors, which are
//     resources of the host executable (`IsNavigationCursorResource`).
// While one stands, nothing of the view's is the HUD's: every message passes, no key is taken,
// the HUD's cursor is not put back, nothing hovers and no redraw is asked for.
//
// ⚠️ THE NAVIGATION CURSOR IS ADMITTED WHEN ARCHICAD SETS IT OVER THE HUD'S, NEVER FROM WHAT A
// GESTURE LEFT BEHIND. A middle-button orbit shows the same cursor, and it is still there on the
// release's own message: read then, it would hand the pointer to Archicad after every orbit and
// break §15 (the HUD's cursor returns at the release). So it counts only when Archicad replaced
// the HUD's cursor with it while no button was held and no navigation was settling -- its idle
// processing does that for as long as the mode lasts -- and, once admitted, it is read LIVE: the
// mode ends with the first cursor that is not a navigation cursor.
//
// ⚠️ NOT YET SEEN LIVE (§7). Which resource a cursor is comes from `GetIconInfoExW`; that Archicad
// loads these cursors as shared resources, so the id is reported at all, is the assumption. Every
// cursor of Archicad's seen over the locked view is therefore said once (`HOSTCURSOR`, with its
// resource and module), and every change of mode (`HOSTMODE`): a run where Orbit still does
// nothing shows which of the two did not hold.

#include "ArchViz/OverlayInputEvents.hpp"
#include "ArchViz/OverlayInputState.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OverlayNavigationWatch.hpp"

#include <cstdio>
#include <cwchar>

namespace geomsrv::archviz::overlayinput::state {

namespace {
using events::PhysicalButtons;

// A cursor of another author's seen over the locked view: which resource, and whether it is one
// Archicad navigates with. Handles of shared resource cursors do not change while the module is
// loaded; the table is session state all the same (§8).
struct Known {
    HCURSOR cursor = nullptr;
    uint32_t resource = 0;
    bool navigation = false;
};
constexpr size_t kKnown = 16;
constexpr uint32_t kSaidMost = 48; // HOSTCURSOR lines a session: the table can evict and re-learn
Known s_known[kKnown];
size_t s_knownNext = 0;
uint32_t s_said = 0;
bool s_cursorMode = false; // a navigation cursor was admitted, and is read live since
HostMode s_lastSaid = HostMode::None;

const wchar_t* BaseName (const wchar_t* path)
{
    const wchar_t* base = path;
    for (const wchar_t* c = path; *c != L'\0'; ++c)
        if (*c == L'\\' || *c == L'/')
            base = c + 1;
    return base;
}

const Known& Classify (HCURSOR cursor)
{
    for (const Known& known : s_known)
        if (known.cursor == cursor && cursor != nullptr)
            return known;
    Known fresh;
    fresh.cursor = cursor;
    wchar_t module[MAX_PATH] = L"?";
    ICONINFOEXW info = {};
    info.cbSize = sizeof (info);
    if (cursor != nullptr && ::GetIconInfoExW (cursor, &info)) {
        // The call hands back copies of the cursor's bitmaps: ours to delete.
        if (info.hbmMask != nullptr)
            ::DeleteObject (info.hbmMask);
        if (info.hbmColor != nullptr)
            ::DeleteObject (info.hbmColor);
        fresh.resource = info.wResID;
        wchar_t host[MAX_PATH] = {};
        ::GetModuleFileNameW (nullptr, host, MAX_PATH);
        wcsncpy_s (module, BaseName (info.szModName), _TRUNCATE);
        fresh.navigation = module[0] != L'\0' && _wcsicmp (module, BaseName (host)) == 0 &&
                           IsNavigationCursorResource (fresh.resource);
    }
    if (s_said < kSaidMost) {
        ++s_said;
        char line[256] = {};
        _snprintf_s (line, sizeof (line), _TRUNCATE,
                     "HOSTCURSOR   a cursor of Archicad's over the locked view: resource #%u of '%ls' -- %s",
                     fresh.resource, module,
                     fresh.navigation     ? "one it navigates with"
                     : fresh.resource == 0 ? "not a shared resource: its mode cannot be read from it"
                                           : "a tool's");
        ArchVizLog (line);
    }
    Known& slot = s_known[s_knownNext];
    s_knownNext = (s_knownNext + 1) % kKnown;
    slot = fresh;
    return slot;
}

const char* NameOf (HostMode mode)
{
    return mode == HostMode::CursorHidden       ? "Archicad hid the cursor (Explore): every move, click and key is its"
           : mode == HostMode::NavigationCursor ? "Archicad shows a navigation cursor (Orbit, Pan, Zoom): the view is its"
                                                : "ended: the locked view is the HUD's again";
}

// Over a panel, a legend or an open menu the HUD owns the pointer whatever Archicad's mode is.
bool OverRegion (const Target& target)
{
    POINT point = {};
    RECT client = {};
    return target.canvas != nullptr && ::GetCursorPos (&point) && ::ScreenToClient (target.canvas, &point) &&
           ::GetClientRect (target.canvas, &client) &&
           target.map.Hit (float (point.x), float (point.y), float (client.right - client.left),
                           float (client.bottom - client.top)) >= 0;
}

} // namespace

HostMode HostPointerMode (const Target& target)
{
    HostCursorState state;
    state.locked = target.map.locked;
    state.hudGesture = router.GetOwner () == Owner::Hud && PhysicalButtons () != 0;
    CURSORINFO info = {};
    info.cbSize = sizeof (info);
    if (state.locked && !state.hudGesture && ::GetCursorInfo (&info)) {
        state.showing = info.flags != 0; // 0: hidden by its thread; CURSOR_SUPPRESSED is touch, not a mode
        state.admitted = s_cursorMode;
        if (state.showing && s_cursorMode) {
            state.navigationCursor = Classify (info.hCursor).navigation;
            state.overRegion = OverRegion (target);
        }
    }
    const HostMode mode = DecideHostMode (state);
    // Admitted, the cursor is read live: the first that is not a navigation cursor, or a region
    // of the HUD's under the pointer, ends it.
    if (s_cursorMode && state.locked && !state.hudGesture && state.showing && mode == HostMode::None)
        s_cursorMode = false;
    if (mode != s_lastSaid) { // each change once (§7): what the next run reads
        s_lastSaid = mode;
        ArchVizLog (std::string ("HOSTMODE     ") + NameOf (mode));
    }
    return mode;
}

bool OfferHostCursor (const Target& target, HCURSOR foreign)
{
    if (s_cursorMode)
        return true;
    if (!Classify (foreign).navigation)
        return false;
    // What a gesture left behind is not a mode (see the file's head).
    if (PhysicalButtons () != 0 || router.GetOwner () != Owner::None ||
        NavigationSettling (::GetTickCount64 (), target.lastNavigationMs) || OverRegion (target))
        return false;
    s_cursorMode = true;
    return true;
}

// ---- the pointer Archicad was last told of ----------------------------------------------
// One timer a view on the input layer's own window, after the redraw timers' ids (1, 2).
constexpr UINT_PTR kLeaveTimer = 3;

void ArmHostLeave (Target& target, HWND inputWindow)
{
    if (inputWindow == nullptr || !target.map.locked)
        return;
    // Set again by every event of the navigation: it fires one settle after the last.
    target.leaveArmed = ::SetTimer (inputWindow, kLeaveTimer + UINT_PTR (ViewOf (target)),
                                    UINT (kNavigationSettleMs + 16), nullptr) != 0;
}

void DisarmHostLeave (Target& target, HWND inputWindow)
{
    if (target.leaveArmed && inputWindow != nullptr)
        ::KillTimer (inputWindow, kLeaveTimer + UINT_PTR (ViewOf (target)));
    target.leaveArmed = false;
}

bool HostLeaveTimer (HWND inputWindow, WPARAM timer)
{
    if (timer < kLeaveTimer || timer > kLeaveTimer + 1)
        return false;
    Target& target = targets[timer - kLeaveTimer];
    // Still Archicad's -- a button held, a mode of its own, a later event's settle: the timer
    // repeats, and the pointer is said to have left once it is not.
    if (target.canvas != nullptr && target.map.locked && (PhysicalButtons () != 0 || NavigatingNatively (target)))
        return true;
    DisarmHostLeave (target, inputWindow);
    const bool shown = target.owner.shown != nullptr && target.owner.shown ();
    if (target.canvas == nullptr || !target.map.locked || !shown) {
        navwatch::NoteHostHover (false); // unlocked or hidden meanwhile: the moves are Archicad's again
        return true;
    }
    TRACKMOUSEEVENT tracking { sizeof (TRACKMOUSEEVENT), TME_CANCEL | TME_HOVER, target.canvas, 0 };
    ::TrackMouseEvent (&tracking);
    ::SendMessageW (target.canvas, WM_MOUSELEAVE, 0, 0);
    navwatch::NoteHostHover (true);
    return true;
}

void ForgetHostMode ()
{
    for (Known& known : s_known)
        known = Known {};
    s_knownNext = 0;
    s_said = 0;
    s_cursorMode = false;
    s_lastSaid = HostMode::None;
}

} // namespace geomsrv::archviz::overlayinput::state
