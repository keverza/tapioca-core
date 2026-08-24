#ifndef EVP_ARCHVIZ_POINTCLOUDHIERARCHY_HPP
#define EVP_ARCHVIZ_POINTCLOUDHIERARCHY_HPP

#include "ArchViz/PointCloudPly.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

struct PointCloudHierarchyNode {
    uint32_t id = 0;
    uint32_t level = 0;
    uint32_t children[2] = { UINT32_MAX, UINT32_MAX };
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    float geometricError = 0.0f;
    std::vector<uint32_t> pointIndices;

    bool IsLeaf () const
    {
        return children[0] == UINT32_MAX;
    }
    size_t Bytes () const
    {
        return pointIndices.capacity () * sizeof (uint32_t);
    }
};

struct PointCloudHierarchy {
    std::vector<PointCloudHierarchyNode> nodes; // parent-before-child upload order
    size_t Bytes () const;
};

bool BuildPointCloudHierarchy (const PointCloudData& cloud, size_t leafCapacity, size_t representativeCapacity,
                               PointCloudHierarchy& hierarchy, std::string& error);

} // namespace archviz
} // namespace geomsrv

#endif
