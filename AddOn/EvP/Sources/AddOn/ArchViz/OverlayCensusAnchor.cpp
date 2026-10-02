// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/OverlayCensusAnchor -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/OverlayCensusAnchor.hpp"

#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace overlaycensusanchor {

namespace {

namespace cen = dxgi::census;
namespace host = dxgi::hostocclusion;

// The census gate wants a median area of 50 px squared and a longest edge of
// 10 px: a transform that collapses the primitive to a point passes any test
// that looks at one corner. A fixed size is invisible across a site plan and
// fills the screen in a bathroom, so it scales -- clamped at both ends.
float ClampAnchorSize (float size)
{
    if (!(size > 0.25f))
        return 0.25f;
    return size > 50.0f ? 50.0f : size;
}

} // namespace

// ⚠️ WITHOUT AN ANCHOR THE CAMERA CAN NEVER LOCK, AND THE FIRST
// VERSION OF THIS RETURNED SILENTLY WHEN IT COULD NOT SET ONE. The census does
// not rank draw groups by what they look like: it PROJECTS a world-space triangle
// through each candidate's own bytes and scores where it lands. The anchor
// defaults to the world ORIGIN with a one-metre triangle, so on any model not
// sitting on 0,0,0 every group scores `anchor inside clip: 0%`, nothing clears
// the eligibility gate, `SetAutoSelect` never fires, and the overlay never draws
// while the log looks perfectly healthy. Run sixty-two reached `Learning` and
// stayed there for a whole session on exactly that.
//
// ⚠️ THE EXTRACTED MODEL'S OWN CENTRE IS THE ANCHOR, AND ASKING
// ARCHICAD FOR A CAMERA IS THE FALLBACK RATHER THAN THE OTHER WAY ROUND. The
// building is where the user is looking, by construction, whatever the
// projection; a camera read adds a dependency on projection settings for a number
// the geometry already answers. The camera path stays only for the moments before
// the first extraction publishes.
bool PointAtView (std::string& how)
{
    const host::Stats stats = host::GetStats ();
    if (stats.boundsValid) {
        const float centre[3] = { (stats.boundsMin[0] + stats.boundsMax[0]) * 0.5f,
                                  (stats.boundsMin[1] + stats.boundsMax[1]) * 0.5f,
                                  (stats.boundsMin[2] + stats.boundsMax[2]) * 0.5f };
        const float dx = stats.boundsMax[0] - stats.boundsMin[0];
        const float dy = stats.boundsMax[1] - stats.boundsMin[1];
        const float dz = stats.boundsMax[2] - stats.boundsMin[2];
        const float diagonal = std::sqrt (dx * dx + dy * dy + dz * dz);
        cen::SetAnchor (centre[0], centre[1], centre[2], ClampAnchorSize (diagonal * 0.1f));
        how = "model centre";
        return true;
    }

    const CameraStart camera = ArchVizPanel::ReadArchicadCamera ();
    if (camera.valid && !camera.orthographic) {
        const float dx = camera.eye[0] - camera.target[0];
        const float dy = camera.eye[1] - camera.target[1];
        const float dz = camera.eye[2] - camera.target[2];
        const float distance = std::sqrt (dx * dx + dy * dy + dz * dz);
        cen::SetAnchor (camera.target[0], camera.target[1], camera.target[2], ClampAnchorSize (distance * 0.05f));
        how = "view target";
        return true;
    }

    how = "no extracted model yet and no readable camera";
    return false;
}

} // namespace overlaycensusanchor
} // namespace archviz
} // namespace geomsrv
