#ifndef EVP_SURFACECRITERIAMETADATA_HPP
#define EVP_SURFACECRITERIAMETADATA_HPP
#include "Geometry/Mesh.hpp"
#include "Geometry/SurfaceCriteria.hpp"
namespace geomsrv {
// Main thread only, bounded batch; no expensive properties/classifications.
void ReadSurfaceCriteriaMetadata (const Snapshot& snapshot, const std::vector<size_t>& targets,
                                  std::map<std::string, surfacecriteria::Element>& elements);
} // namespace geomsrv
#endif
