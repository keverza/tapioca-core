#ifndef GEOMETRYSERVER_SURFACEATTRIBUTEMAP_HPP
#define GEOMETRYSERVER_SURFACEATTRIBUTEMAP_HPP

#include "ExposedSurface.hpp"

namespace geomsrv::exposed {
struct SurfaceSelection {
    std::map<int32_t, int32_t> poolToSurface; // zero is explicitly unmapped
    std::optional<std::set<int32_t>> materialPoolFilter;
};

// Resolve only explicitly supplied source faces against their capture's original
// surface provenance/catalog. No name matching, pool == attribute assumptions or
// live project reads. Several pool entries may map to one surface, but conflicting
// assignments within a pool refuse rather than filter the wrong fragments.
// Unknown surfaces are allowed only for an unfiltered diagnostic calculation.
bool ResolveSurfaceSelection (const Snapshot& snapshot, const std::vector<FaceKey>& faces,
                              const std::optional<std::set<int32_t>>& surfaceAttributes, SurfaceSelection& selection,
                              std::string& error);
} // namespace geomsrv::exposed
#endif
