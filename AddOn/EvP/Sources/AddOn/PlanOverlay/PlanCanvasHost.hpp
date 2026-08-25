#ifndef EVP_PLANOVERLAY_PLANCANVASHOST_HPP
#define EVP_PLANOVERLAY_PLANCANVASHOST_HPP

#include "PlanOverlay/OverlayOwnership.hpp"
#include "PlanOverlay/OverlayWindow.hpp"

#include <windows.h>

namespace geomsrv {
namespace planoverlay {

enum class SessionStart {
    Opened,
    Reused,
    NotFloorPlan,
    MainWindowUnavailable,
    CanvasUnavailable,
    OverlayInUse,
    CreateFailed
};

struct Session {
    HWND window = nullptr;
    SessionOwnership ownership;
};

// Main-thread only. Opens over the active floor-plan canvas or borrows an
// already matching overlay without taking ownership of it.
SessionStart BeginCurrentPlanSession (Owner owner, const Style& style, UINT trackingIntervalMs, Session& session);
void EndSession (Session& session);

} // namespace planoverlay
} // namespace geomsrv

#endif
