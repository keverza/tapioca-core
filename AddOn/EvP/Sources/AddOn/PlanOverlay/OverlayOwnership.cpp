#include "PlanOverlay/OverlayOwnership.hpp"

namespace geomsrv {
namespace planoverlay {

ReleaseAction DetermineReleaseAction (const SessionOwnership& session, Owner currentOwner, bool sameWindow)
{
    if (!sameWindow)
        return ReleaseAction::None;
    if (session.ownsWindow)
        return currentOwner == session.owner ? ReleaseAction::DestroyWindow : ReleaseAction::None;
    if (session.startedTracking)
        return ReleaseAction::StopTracking;
    return ReleaseAction::None;
}

} // namespace planoverlay
} // namespace geomsrv
