// Link stub for the Archicad half of the viewpoint bake.
//
// ArchViz/ViewpointBake.cpp makes an Archicad Fill through ACAPI and can never
// join the offline build, while DiligentHudViewpoint.cpp -- which the offline
// gate DOES compile -- offers the Bake button. This answers as production does
// before any bake: nothing baked yet. The polygon it would bake is tested
// through evp::sunstudy::ViewpointPlanOutline instead.
//
// ⚠️ ADD A FUNCTION HERE ONLY WHEN A LINKED SOURCE CALLS IT.

#include "ArchViz/ViewpointBake.hpp"

namespace geomsrv::archviz::viewpointbake {

void RequestBake (std::vector<std::array<double, 2>>)
{
}

Baked LastBake ()
{
    return {};
}

} // namespace geomsrv::archviz::viewpointbake
