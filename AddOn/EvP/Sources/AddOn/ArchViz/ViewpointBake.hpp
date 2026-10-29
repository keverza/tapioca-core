#ifndef EVP_ARCHVIZ_VIEWPOINTBAKE_HPP
#define EVP_ARCHVIZ_VIEWPOINTBAKE_HPP

// ArchViz/ViewpointBake -- a plan isovist or field of view baked into the project as
// an Archicad Fill on the floor plan's active story, with the Fill tool's current
// settings, in one undo step. The HUD asks from the render thread; the Fill is
// made later on the main thread (SelectionMetadata's `Later`), and the HUD reads
// the outcome by its serial.
//
// ANY THREAD: RequestBake, LastBake.

#include "SunStudy/ViewpointStudy.hpp"

#include <cstdint>
#include <string>

namespace geomsrv::archviz::viewpointbake {

// `contour`: evp::sunstudy::ViewpointBakeContour, metres in plan.
void RequestBake (evp::sunstudy::ViewpointPlanContour contour);

struct Baked {
    uint64_t serial = 0; // grows with each finished bake
    std::string story;   // the story it was placed on
    std::string error;   // empty when the Fill was made
};

Baked LastBake ();

} // namespace geomsrv::archviz::viewpointbake

#endif
