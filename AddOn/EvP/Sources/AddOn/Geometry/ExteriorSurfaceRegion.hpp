#ifndef GEOMETRYSERVER_EXTERIORSURFACEREGION_HPP
#define GEOMETRYSERVER_EXTERIORSURFACEREGION_HPP

#include "ExposedSurface.hpp"
#include <array>

namespace geomsrv::exposed {
// Exterior is an operator assertion at each seed, not an inference from material,
// normal direction or camera visibility. MVP expansion stays on that supporting
// plane and crosses positive-length shared boundaries/positive-area overlaps only.
struct ExteriorSeed {
    size_t meshIndex = 0;
    std::array<double, 3> point {}, normal {};
};
bool ResolveFacadeRegion (const Snapshot& snapshot, const std::vector<size_t>& targets,
                          const std::vector<ExteriorSeed>& seeds, std::vector<FaceKey>& faces, std::string& error,
                          const Limits& limits = {}, const std::function<bool ()>& cancelled = {});
} // namespace geomsrv::exposed
#endif
