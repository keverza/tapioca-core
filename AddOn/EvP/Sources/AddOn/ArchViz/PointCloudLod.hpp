#ifndef EVP_ARCHVIZ_POINTCLOUDLOD_HPP
#define EVP_ARCHVIZ_POINTCLOUDLOD_HPP

#include "ArchViz/PointCloudHierarchy.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {

struct PointCloudLodCamera {
    float eye[3] = {};
    float viewportHeightPixels = 1.0f;
    float verticalFieldOfViewRadians = 1.0f;
    float orthographicHeight = 1.0f;
    bool orthographic = false;
};

// Chooses parent representatives or child nodes from a stable camera, never a
// jittered projection. The returned IDs are in hierarchy order and fit the
// requested point budget unless even the root representation exceeds it.
std::vector<uint32_t> SelectPointCloudLod (const PointCloudHierarchy& hierarchy, const PointCloudLodCamera& camera,
                                           float pixelError, size_t pointBudget);

} // namespace archviz
} // namespace geomsrv

#endif
