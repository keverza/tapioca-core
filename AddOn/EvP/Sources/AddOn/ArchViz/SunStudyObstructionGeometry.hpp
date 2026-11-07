#ifndef EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONGEOMETRY_HPP
#define EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONGEOMETRY_HPP

// Pure world-space geometry for single-day exact obstruction attribution.
// Whole blockers contribute their CAD/source-boundary wireframe once; only the
// measured hit triangles are filled. Whole-day hits retain their clock minute.

#include "ArchViz/GhPreviewGeometry.hpp"
#include "Geometry/Mesh.hpp"
#include "SunStudy/SunStudyObstruction.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace geomsrv::archviz {

constexpr uint32_t kSunObstructionPink = 0xF240BFFFu;

struct SunObstructionGeometry {
    GhPreviewDrawables drawables;
    size_t blockerObjects = 0;
    size_t timedParts = 0; // unique object/triangle/minute fills
    std::string error;
};

uint32_t SunObstructionTimeColour (int minute);

// `wholeDay` selects the time gradient; otherwise every fill is pink. Multiple
// selected cells that name the same part at the same step share one fill.
// Timed hits are emitted chronologically; a part blocking at several times has
// one fill per time, with the latest foremost in the ordinary draw order.
bool BuildSunObstructionGeometry (const geomsrv::Snapshot& snapshot,
                                  const std::vector<evp::sunstudy::SunObstructionResult>& steps, bool wholeDay,
                                  SunObstructionGeometry& result);

} // namespace geomsrv::archviz

#endif
