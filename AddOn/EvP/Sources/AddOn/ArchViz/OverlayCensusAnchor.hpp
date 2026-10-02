#ifndef EVP_ARCHVIZ_OVERLAYCENSUSANCHOR_HPP
#define EVP_ARCHVIZ_OVERLAYCENSUSANCHOR_HPP

// ArchViz/OverlayCensusAnchor -- where the 3D overlay points the census's scorer while the
// camera is learned (InjectedOverlayRuntime: at arm, and every tick until a lock).
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md BEFORE EDITING. The anchor
// is what every candidate camera is scored against (§4); production sets it itself and
// depends on no diagnostic having done so (§9).
//
// MAIN THREAD.

#include <string>

namespace geomsrv {
namespace archviz {
namespace overlaycensusanchor {

// Point the census's scorer at something certainly on screen. True with `how` naming what
// it stands on; false with `how` saying why there is nothing yet.
bool PointAtView (std::string& how);

} // namespace overlaycensusanchor
} // namespace archviz
} // namespace geomsrv

#endif
