#include "APIEnvir.h"
#include "ACAPinc.h"

#include "PlanOverlay/PlanCanvasHost.hpp"

namespace geomsrv {
namespace planoverlay {

namespace {

struct PlanCanvasTarget {
    HWND parent = nullptr;
    HWND canvas = nullptr;
    RECT rectInParent {};
    // WHICH floor plan this canvas is showing, captured at the same moment the
    // canvas HWND is. The two have to be read together: Archicad reuses one
    // document window for several databases, so an HWND on its own does not say
    // which one is in it, and an overlay that only remembers the HWND keeps
    // drawing after the user opens a schedule in the same frame.
    OverlayWindowId window;
};

HWND ChildAtPoint (HWND parent, POINT screenPoint, bool skipTransparent, bool excludeOverlay)
{
    for (HWND child = GetWindow (parent, GW_CHILD); child != nullptr; child = GetWindow (child, GW_HWNDNEXT)) {
        if (!IsWindowVisible (child) || !IsWindowEnabled (child))
            continue;
        if (skipTransparent && (GetWindowLongPtrW (child, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0)
            continue;
        if (excludeOverlay && IsOverlayWindow (child))
            continue;
        RECT rect {};
        GetWindowRect (child, &rect);
        if (PtInRect (&rect, screenPoint))
            return child;
    }
    return nullptr;
}

SessionStart FindCurrentPlanCanvas (PlanCanvasTarget& target)
{
    API_WindowInfo windowInfo {};
    if (ACAPI_Window_GetCurrentWindow (&windowInfo) != NoError || windowInfo.typeID != APIWind_FloorPlanID)
        return SessionStart::NotFloorPlan;
    target.window = CurrentWindowId ();

    HWND mainWindow = ACAPI_GetMainWindow ();
    if (mainWindow == nullptr || !IsWindow (mainWindow))
        return SessionStart::MainWindowUnavailable;

    RECT mainClient {};
    if (!GetClientRect (mainWindow, &mainClient))
        return SessionStart::CanvasUnavailable;
    POINT screenPoint { (mainClient.left + mainClient.right) / 2, (mainClient.top + mainClient.bottom) / 2 };
    ClientToScreen (mainWindow, &screenPoint);

    const std::vector<HWND> chain = DescendWindowChain (mainWindow, screenPoint, false, true);
    if (chain.size () < 2)
        return SessionStart::CanvasUnavailable;
    target.canvas = chain.back ();
    target.parent = GetParent (target.canvas);
    if (target.parent == nullptr || !IsWindow (target.parent))
        return SessionStart::CanvasUnavailable;

    GetWindowRect (target.canvas, &target.rectInParent);
    MapWindowPoints (HWND_DESKTOP, target.parent, reinterpret_cast<LPPOINT> (&target.rectInParent), 2);
    if (target.rectInParent.right <= target.rectInParent.left || target.rectInParent.bottom <= target.rectInParent.top)
        return SessionStart::CanvasUnavailable;
    return SessionStart::Opened;
}

} // namespace

std::vector<HWND> DescendWindowChain (HWND root, POINT screenPoint, bool skipTransparent, bool excludeOverlay)
{
    std::vector<HWND> chain;
    if (root == nullptr || !IsWindow (root))
        return chain;

    HWND current = root;
    chain.push_back (current);
    for (int guard = 0; guard < 64; ++guard) {
        HWND child = nullptr;
        if (excludeOverlay) {
            child = ChildAtPoint (current, screenPoint, skipTransparent, true);
        }
        else {
            UINT flags = CWP_SKIPINVISIBLE | CWP_SKIPDISABLED;
            if (skipTransparent)
                flags |= CWP_SKIPTRANSPARENT;
            POINT clientPoint = screenPoint;
            ScreenToClient (current, &clientPoint);
            child = ChildWindowFromPointEx (current, clientPoint, flags);
        }
        if (child == nullptr || child == current)
            break;
        chain.push_back (child);
        current = child;
    }
    return chain;
}

SessionStart BeginCurrentPlanSession (Owner owner, const Style& style, UINT trackingIntervalMs, Session& session)
{
    if (session.window != nullptr && Current () == session.window)
        return SessionStart::Reused;

    PlanCanvasTarget target;
    const SessionStart targetStatus = FindCurrentPlanCanvas (target);
    if (targetStatus != SessionStart::Opened)
        return targetStatus;

    HWND overlay = Current ();
    bool ownsWindow = false;
    if (overlay != nullptr) {
        if (Canvas () != target.canvas)
            return SessionStart::OverlayInUse;
    }
    else {
        overlay = Create (target.parent, target.canvas, target.rectInParent, style, owner);
        if (overlay == nullptr)
            return SessionStart::CreateFailed;
        ownsWindow = true;
    }

    // ⚠️ BOUND BEFORE TRACKING STARTS. The first timer tick compares against
    // this; binding afterwards would let one tick run against whatever window
    // happened to be in front, and a tick that projects into the wrong window is
    // the one picture a user would trust and should not.
    BindWindow (target.window);

    const bool startedTracking = !GetTrackStats ().tracking;
    if (startedTracking)
        SetTracking (true, trackingIntervalMs);
    session.window = overlay;
    session.ownership = { owner, ownsWindow, startedTracking };
    return ownsWindow ? SessionStart::Opened : SessionStart::Reused;
}

void EndSession (Session& session)
{
    const bool sameWindow = session.window != nullptr && Current () == session.window;
    switch (DetermineReleaseAction (session.ownership, CurrentOwner (), sameWindow)) {
        case ReleaseAction::DestroyWindow:
            DestroyOwned (session.ownership.owner);
            break;
        case ReleaseAction::StopTracking:
            SetTracking (false, 0);
            break;
        case ReleaseAction::None:
            break;
    }
    session = {};
}

} // namespace planoverlay
} // namespace geomsrv
