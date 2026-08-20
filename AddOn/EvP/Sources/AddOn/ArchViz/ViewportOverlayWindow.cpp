#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ViewportOverlayWindow.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog

#include <cstdio>
#include <cwchar>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace viewportoverlay {

namespace {

constexpr const wchar_t* kOverlayClass = L"Tapioca_ArchVizOverlay_Window";
constexpr UINT_PTR kTrackTimerId = 0xE7C2;

HWND s_overlay = nullptr;
HWND s_target = nullptr;
RECT s_rect = {};
bool s_classRegistered = false;
UINT_PTR s_timer = 0;
ResizeCallback s_onResize = nullptr;
OverlayStats s_stats;
OverlayAttach s_attach = OverlayAttach::Popup;

// Every overlay we created and did not destroy ourselves. ⚠️ NOT JUST
// `s_overlay`: PlanOverlay found the hard way that Archicad can destroy an
// overlay for us when the window it was parented to closes, after which a NEW
// one can exist while the old handle is merely stale. On unload the only safe
// question is "did we create it", not "is it the current one". This one is a
// POPUP with no Archicad parent, so that particular route is closed -- but the
// bookkeeping costs nothing and the failure it prevents is a crash on exit.
std::vector<HWND> s_created;

bool IsOurs (HWND hwnd)
{
    wchar_t cls[256] = {};
    GetClassNameW (hwnd, cls, 255);
    return wcscmp (cls, kOverlayClass) == 0;
}

std::string ClassNameOf (HWND hwnd)
{
    char cls[128] = {};
    if (hwnd != nullptr && IsWindow (hwnd))
        GetClassNameA (hwnd, cls, (int) sizeof (cls) - 1);
    return std::string (cls);
}

// Put the overlay in front OF WHAT IT COVERS, which means something different
// in each attach mode.
//
// ⚠️ A POPUP MUST BE AT HWND_TOP TO BE SEEN AT ALL, MEASURED 2026-08-13. The
// obvious refinement -- insert it directly above the top-level ancestor of the
// window it covers, so Archicad's callouts stay above -- was built and reported
// back as "invisible with occasional short blips". The blips were this poll
// re-asserting the position and losing again immediately. So a top-level
// overlay here is not merely competing for order; anything below the very top
// gets composited over. That is why the popup mode CANNOT satisfy the callout
// requirement, and why the child modes exist.
//
// ⚠️ HWND_TOP MEANS SOMETHING ELSE ENTIRELY FOR A CHILD, and this is the whole
// point of the child modes: it orders the window among its SIBLINGS, inside the
// parent. Above the canvas, and still underneath every top-level palette,
// tooltip and callout, because Windows always draws a top-level window above
// another top-level window's children. Nothing has to out-race anything.
void PlaceAboveTarget (HWND overlay, HWND /*target*/)
{
    if (overlay == nullptr || !IsWindow (overlay))
        return;

    // SWP_NOOWNERZORDER: never reposition Archicad's main window, which is the
    // popup's owner and the one window this must not move.
    SetWindowPos (overlay, HWND_TOP, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// This DLL's module handle — NOT the host EXE's. The class and the window must
// belong to the module that owns the WndProc, or UnregisterClassW cannot take
// the class down at unload and a stale procedure address survives in Archicad's
// class table.
LRESULT CALLBACK OverlayProc (HWND, UINT, WPARAM, LPARAM);

HMODULE OwnModule ()
{
    static HMODULE mod = nullptr;
    if (mod == nullptr) {
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR> (&OverlayProc), &mod);
    }
    return mod;
}

LRESULT CALLBACK OverlayProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCHITTEST:
            // ⚠️ BELT AND BRACES WITH WS_EX_TRANSPARENT, AND BOTH ARE WANTED.
            // The style is what actually routes the mouse past a layered
            // top-level window; this is what answers the question if anything
            // ever asks us directly. They cannot disagree.
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            // Nothing may paint here: DirectComposition supplies every pixel and
            // a GDI erase would fight it.
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint (hwnd, &ps);
            EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            break;
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}

// The client rect of `hwnd`, in screen pixels.
RECT ClientRectOnScreen (HWND hwnd)
{
    RECT r = {};
    if (hwnd == nullptr || !IsWindow (hwnd))
        return r;
    GetClientRect (hwnd, &r);
    POINT topLeft = {r.left, r.top};
    POINT bottomRight = {r.right, r.bottom};
    ClientToScreen (hwnd, &topLeft);
    ClientToScreen (hwnd, &bottomRight);
    r.left = topLeft.x;
    r.top = topLeft.y;
    r.right = bottomRight.x;
    r.bottom = bottomRight.y;
    return r;
}

bool SameRect (const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// The deepest visible child at `screenPt`, skipping our own windows.
//
// ⚠️ SKIPPING OUR OWN IS NOT TIDINESS -- PlanOverlay paid for it. Once the
// overlay exists it is the deepest window at that pixel, so a plain descent
// STOPS there and the caller's "deepest window" is the overlay's parent, one
// level too shallow. Every repeated show then attached a level higher than the
// last, which is exactly what happened live: run 1 covered the plan, run 2 the
// document window, run 3 the MDI client. Our overlay is a top-level popup rather
// than a child now, so it is not in this tree at all -- but a DG palette hosting
// the other viewer IS, and the same reasoning applies to it.
HWND DeepestChildAt (HWND root, POINT screenPt)
{
    HWND current = root;
    for (int guard = 0; guard < 64; ++guard) {
        HWND found = nullptr;
        for (HWND child = GetWindow (current, GW_CHILD); child != nullptr;
             child = GetWindow (child, GW_HWNDNEXT)) {
            if (!IsWindowVisible (child) || IsOurs (child))
                continue;
            RECT r = {};
            GetWindowRect (child, &r);
            if (PtInRect (&r, screenPt)) {
                found = child;
                break;   // GW_CHILD is the TOPMOST child; this is z-order
            }
        }
        if (found == nullptr || found == current)
            break;
        current = found;
    }
    return current == root ? nullptr : current;
}

}   // namespace

OverlayTarget FindOverlayTarget ()
{
    OverlayTarget target;

    HWND main = ACAPI_GetMainWindow ();
    if (main == nullptr || !IsWindow (main)) {
        target.how = "ACAPI_GetMainWindow returned nothing Windows recognises";
        return target;
    }

    const RECT mainClient = ClientRectOnScreen (main);
    if (mainClient.right - mainClient.left < 64 || mainClient.bottom - mainClient.top < 64) {
        target.how = "Archicad's main window has no usable client area";
        return target;
    }
    const POINT centre = {(mainClient.left + mainClient.right) / 2,
                          (mainClient.top + mainClient.bottom) / 2};

    HWND deepest = DeepestChildAt (main, centre);
    if (deepest == nullptr) {
        target.how = "no child window of Archicad's main window covers the centre of its "
                     "client area";
        return target;
    }

    target.window = deepest;
    target.screenRect = ClientRectOnScreen (deepest);
    target.windowClass = ClassNameOf (deepest);
    target.valid = (target.screenRect.right - target.screenRect.left) >= 16 &&
                   (target.screenRect.bottom - target.screenRect.top) >= 16;
    // ⚠️ IT DOES NOT CLAIM TO BE THE 3D WINDOW, and it must not. There is no
    // DevKit call that hands back the 3D view's HWND, and deciding from a class
    // name would be a guess (CLAUDE.md: never cite an API you have not grepped,
    // and the same discipline applies to inferring one). What is reported is
    // exactly what was measured: the frontmost document canvas. If the floor plan
    // is in front, the overlay lands on the floor plan and says so.
    target.how = std::string ("frontmost document canvas under the centre of Archicad's main "
                              "window, class '") + target.windowClass + "'";
    if (!target.valid)
        target.how += " -- but it is too small to overlay";
    return target;
}

HWND Create (const OverlayTarget& target, OverlayAttach attach, std::string& error)
{
    Destroy ();

    if (!target.valid || target.window == nullptr || !IsWindow (target.window)) {
        error = "no usable window to overlay: " + target.how;
        return nullptr;
    }

    if (!s_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = OwnModule ();
        wc.lpszClassName = kOverlayClass;
        // ⚠️ NO BACKGROUND BRUSH. A brush is a GDI fill, and a GDI fill on a
        // NOREDIRECTIONBITMAP window is at best wasted and at worst the thing
        // DWM composites instead of our visual.
        wc.hbrBackground = nullptr;
        if (RegisterClassExW (&wc) == 0) {
            const DWORD e = GetLastError ();
            if (e != ERROR_CLASS_ALREADY_EXISTS) {
                error = "RegisterClassEx failed, GetLastError=" + std::to_string (e);
                return nullptr;
            }
        }
        s_classRegistered = true;
    }

    // ⚠️ ALL FIVE EXTENDED STYLES ARE LOAD-BEARING. See the header for the two
    // that are counter-intuitive (LAYERED is for INPUT, NOREDIRECTIONBITMAP is
    // for PIXELS, and they must coexist).
    //
    //   NOREDIRECTIONBITMAP  no GDI surface for DWM to composite instead of ours
    //   LAYERED              makes the mouse pass through, across windows
    //   TRANSPARENT          the actual pass-through
    //   NOACTIVATE           never steal focus, never appear in Alt-Tab
    //   TOOLWINDOW           keeps it out of the taskbar
    const bool asChild = attach != OverlayAttach::Popup;

    // NOREDIRECTIONBITMAP and TRANSPARENT are in every mode: the first is what
    // lets the composition surface reach DWM at all, the second is the
    // hit-testing rule. TOOLWINDOW and the owner are meaningless on a child.
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    if (!asChild)
        exStyle |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    else if (attach == OverlayAttach::ChildLayered)
        exStyle |= WS_EX_LAYERED;

    const int width = target.screenRect.right - target.screenRect.left;
    const int height = target.screenRect.bottom - target.screenRect.top;

    // ⚠️ ARCHICAD'S MAIN WINDOW IS THE *OWNER*, AND THAT IS WHAT KEEPS THE
    // OVERLAY ON SCREEN. For a WS_POPUP the `hWndParent` argument is an OWNER,
    // not a parent: Windows guarantees an owned window stays ABOVE its owner in
    // z-order for as long as both exist.
    //
    // The first live run (2026-08-11) passed nullptr here, and the overlay was
    // reported as disappearing "the instant I navigate". That is exactly what an
    // UNOWNED top-level popup does -- navigating activates Archicad's window,
    // Windows raises it, and the popup is left behind it with nothing to bring it
    // back. The picture was still being rendered and presented the whole time
    // (11,118 frames in that run); it was simply underneath.
    //
    // ⚠️ AND IT IS WHY HWND_TOPMOST IS STILL NOT USED. Ownership solves the
    // z-order against ARCHICAD; topmost would also put us over every other
    // application, and over Archicad's own modals.
    HWND owner = ACAPI_GetMainWindow ();
    if (owner != nullptr && !IsWindow (owner))
        owner = nullptr;

    // ⚠️ A CHILD IS POSITIONED IN ITS PARENT'S CLIENT COORDINATES, so it starts
    // at 0,0 and spans the whole view -- NOT at the screen rect a popup uses.
    // Getting this wrong offsets the overlay by the window's screen position,
    // which on a maximised Archicad is a small enough error to look like a
    // camera fault rather than a coordinate-space one.
    const DWORD style = asChild ? (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS) : WS_POPUP;
    const int left = asChild ? 0 : target.screenRect.left;
    const int top  = asChild ? 0 : target.screenRect.top;
    HWND parentOrOwner = asChild ? target.window : owner;

    HWND hwnd = CreateWindowExW (exStyle, kOverlayClass, L"Tapioca 3D overlay", style,
                                 left, top, width, height,
                                 parentOrOwner, nullptr, OwnModule (), nullptr);
    if (hwnd == nullptr) {
        error = "CreateWindowEx failed, GetLastError=" + std::to_string (GetLastError ());
        return nullptr;
    }

    // ⚠️ SW_SHOWNOACTIVATE, NEVER SW_SHOW. Taking activation from Archicad
    // mid-session changes what Archicad is doing -- and an overlay that steals
    // focus every time it appears is worse than no overlay.
    ShowWindow (hwnd, SW_SHOWNOACTIVATE);
    s_attach = attach;
    // ⚠️ NOT HWND_TOPMOST -- see PlaceAboveTarget for why, and for why the
    // narrower position that would keep Archicad's callouts visible does not
    // work either.
    PlaceAboveTarget (hwnd, target.window);

    s_overlay = hwnd;
    s_target = target.window;
    s_rect = target.screenRect;
    s_created.push_back (hwnd);

    s_stats = OverlayStats {};
    s_stats.active = true;
    s_stats.overlay = hwnd;
    s_stats.target = target.window;
    s_stats.screenRect = target.screenRect;
    s_stats.width = uint32_t (width);
    s_stats.height = uint32_t (height);
    s_stats.targetClass = target.windowClass;
    s_stats.attach = int (attach);
    s_stats.how = target.how;

    ArchVizLog ("ArchViz overlay window: " + std::to_string (width) + "x" +
                std::to_string (height) + " at " + std::to_string (target.screenRect.left) + "," +
                std::to_string (target.screenRect.top) + " over " + target.how);
    return hwnd;
}

void Destroy ()
{
    SetTracking (false, 0, nullptr);
    for (HWND h : s_created) {
        if (h != nullptr && IsWindow (h) && IsOurs (h))
            DestroyWindow (h);
    }
    s_created.clear ();
    s_overlay = nullptr;
    s_target = nullptr;
    s_rect = RECT {};
    s_stats.active = false;
    s_stats.overlay = nullptr;
}

void Shutdown ()
{
    Destroy ();
    if (s_classRegistered) {
        UnregisterClassW (kOverlayClass, OwnModule ());
        s_classRegistered = false;
    }
}

HWND Current () { return s_overlay; }

void SetVisible (bool visible)
{
    if (s_overlay == nullptr)
        return;
    // ⚠️ SW_SHOWNOACTIVATE, NEVER SW_SHOW. The overlay is an owned popup over
    // Archicad's document window; activating it takes the focus away from the
    // window the user is navigating, which stops the navigation mid-gesture.
    ShowWindow (s_overlay, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}
OverlayStats Stats () { return s_stats; }

namespace {

void CALLBACK TrackTimerProc (HWND, UINT, UINT_PTR, DWORD)
{
    if (s_overlay == nullptr || !IsWindow (s_overlay))
        return;
    ++s_stats.trackPolls;

    // The target going away is ordinary: the user closed the 3D window. Take the
    // overlay down with it rather than leaving a picture floating over whatever
    // is underneath now.
    if (s_target == nullptr || !IsWindow (s_target) || !IsWindowVisible (s_target)) {
        ArchVizLog ("ArchViz overlay: the window it was covering is gone; closing the overlay");
        Destroy ();
        return;
    }

    // ⚠️ Z-ORDER IS RE-ASSERTED EVERY POLL, NOT ONLY ON A MOVE, because Archicad
    // raises its own document windows constantly and the overlay would otherwise
    // sink behind the canvas with nothing in the log to say so.
    //
    // ⚠️ ONLY THE POPUP NEEDS THIS, and it is also why the popup covers
    // Archicad's callouts (PLAT-RE64): it has to be at HWND_TOP to be visible.
    // A CHILD keeps its place among its siblings without being re-asserted, and
    // re-asserting it would be the same mistake one level down -- it would climb
    // over any child of the view that Archicad legitimately raised.
    if (s_attach == OverlayAttach::Popup)
        PlaceAboveTarget (s_overlay, s_target);

    const RECT now = ClientRectOnScreen (s_target);
    if (SameRect (now, s_rect))
        return;

    // ⚠️ A CHILD IS ALREADY MOVED BY WINDOWS when its parent moves -- it is
    // positioned in parent client coordinates, so a window drag changes the
    // screen rect and not the child's own position. Only a SIZE change is ours
    // to apply, and applying the screen rect to a child would fling it off by
    // the parent's position on screen.
    const bool asChild = s_attach != OverlayAttach::Popup;

    ++s_stats.trackMoves;
    const int width = now.right - now.left;
    const int height = now.bottom - now.top;
    const bool resized = (width != s_rect.right - s_rect.left) ||
                         (height != s_rect.bottom - s_rect.top);
    s_rect = now;
    s_stats.screenRect = now;
    s_stats.width = uint32_t (width > 0 ? width : 0);
    s_stats.height = uint32_t (height > 0 ? height : 0);

    if (asChild)
        SetWindowPos (s_overlay, nullptr, 0, 0, width, height,
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    else
        SetWindowPos (s_overlay, nullptr, now.left, now.top, width, height,
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);

    // ⚠️ ONLY ON A REAL SIZE CHANGE. Recreating a composition swap chain's
    // buffers is not free, and Archicad's window rect twitches by a pixel during
    // ordinary layout; resizing on every move would rebuild the back buffers
    // several times a second while the user drags the application window.
    if (resized && s_onResize != nullptr && width > 0 && height > 0)
        s_onResize (uint32_t (width), uint32_t (height));
}

}   // namespace

void SetTracking (bool enable, UINT intervalMs, ResizeCallback onResize)
{
    if (s_timer != 0) {
        KillTimer (nullptr, s_timer);
        s_timer = 0;
    }
    s_onResize = nullptr;
    if (!enable || s_overlay == nullptr)
        return;
    if (intervalMs < 10)
        intervalMs = 10;
    s_onResize = onResize;
    // nullptr window: the WM_TIMER is posted to THIS thread's queue and
    // DispatchMessage invokes the callback here, so no window ownership is
    // involved and nothing needs subclassing. Archicad pumps the queue.
    s_timer = SetTimer (nullptr, 0, intervalMs, TrackTimerProc);
    if (s_timer == 0)
        ArchVizLog ("ArchViz overlay: SetTimer failed; the overlay will NOT follow the window "
                    "it covers, so moving or resizing Archicad leaves it behind");
}

}   // namespace viewportoverlay
}   // namespace archviz
}   // namespace geomsrv
