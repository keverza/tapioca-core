#include "PlanOverlay/OverlayBinding.hpp"

namespace geomsrv {
namespace planoverlay {

OverlayTrackDecision DecideTrackTick (const OverlayWindowId& bound, const OverlayWindowId& current,
                                      bool currentlyVisible)
{
    OverlayTrackDecision decision;

    // Unbound: the behaviour that existed before overlays knew which window they
    // belonged to. Kept rather than treated as "suspend", because a caller that
    // never recorded a window would otherwise get a blank overlay and read it as
    // the overlay being broken.
    if (!bound.Known ()) {
        decision.track = true;
        decision.visible = true;
    }
    // ⚠️ AN UNREADABLE CURRENT WINDOW SUSPENDS RATHER THAN CARRYING ON. Not
    // knowing which window is in front is exactly the moment not to project
    // geometry into it, and it is also what a project being closed looks like
    // from inside a timer that is still running.
    else if (!current.Known ()) {
        decision.track = false;
        decision.visible = false;
    }
    else {
        const bool mine = current == bound;
        decision.track = mine;
        decision.visible = mine;
    }

    decision.visibilityChanged = decision.visible != currentlyVisible;
    return decision;
}

} // namespace planoverlay
} // namespace geomsrv
