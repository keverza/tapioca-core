#include "ArchViz/PointCloudHierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace geomsrv {
namespace archviz {

namespace {

void SetBounds (const PointCloudData& cloud, const std::vector<uint32_t>& indices, PointCloudHierarchyNode& node)
{
    for (size_t axis = 0; axis < 3; ++axis) {
        node.boundsMin[axis] = std::numeric_limits<float>::max ();
        node.boundsMax[axis] = std::numeric_limits<float>::lowest ();
    }
    for (uint32_t index : indices) {
        const PointCloudVertex& point = cloud.vertices[index];
        for (size_t axis = 0; axis < 3; ++axis) {
            node.boundsMin[axis] = std::min (node.boundsMin[axis], point.position[axis]);
            node.boundsMax[axis] = std::max (node.boundsMax[axis], point.position[axis]);
        }
    }
    float diagonalSquared = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float extent = node.boundsMax[axis] - node.boundsMin[axis];
        diagonalSquared += extent * extent;
    }
    node.geometricError = std::sqrt (diagonalSquared);
}

void SetRepresentatives (const std::vector<uint32_t>& indices, size_t capacity, PointCloudHierarchyNode& node)
{
    const size_t count = std::min (capacity, indices.size ());
    node.pointIndices.reserve (count);
    for (size_t i = 0; i < count; ++i)
        node.pointIndices.push_back (indices[(i * indices.size ()) / count]);
}

uint32_t BuildNode (const PointCloudData& cloud, std::vector<uint32_t> indices, size_t leafCapacity,
                    size_t representativeCapacity, uint32_t level, PointCloudHierarchy& hierarchy)
{
    const uint32_t id = uint32_t (hierarchy.nodes.size ());
    hierarchy.nodes.emplace_back ();
    hierarchy.nodes[id].id = id;
    hierarchy.nodes[id].level = level;
    SetBounds (cloud, indices, hierarchy.nodes[id]);

    if (indices.size () <= leafCapacity || hierarchy.nodes[id].geometricError <= 0.0f) {
        hierarchy.nodes[id].pointIndices = std::move (indices);
        return id;
    }

    size_t splitAxis = 0;
    for (size_t axis = 1; axis < 3; ++axis) {
        const float extent = hierarchy.nodes[id].boundsMax[axis] - hierarchy.nodes[id].boundsMin[axis];
        const float best = hierarchy.nodes[id].boundsMax[splitAxis] - hierarchy.nodes[id].boundsMin[splitAxis];
        if (extent > best)
            splitAxis = axis;
    }
    std::stable_sort (indices.begin (), indices.end (), [&cloud, splitAxis] (uint32_t left, uint32_t right) {
        const float a = cloud.vertices[left].position[splitAxis];
        const float b = cloud.vertices[right].position[splitAxis];
        return a < b || (a == b && left < right);
    });
    SetRepresentatives (indices, representativeCapacity, hierarchy.nodes[id]);

    const auto middle = indices.begin () + ptrdiff_t (indices.size () / 2);
    std::vector<uint32_t> left (indices.begin (), middle);
    std::vector<uint32_t> right (middle, indices.end ());
    const uint32_t leftId =
        BuildNode (cloud, std::move (left), leafCapacity, representativeCapacity, level + 1, hierarchy);
    const uint32_t rightId =
        BuildNode (cloud, std::move (right), leafCapacity, representativeCapacity, level + 1, hierarchy);
    hierarchy.nodes[id].children[0] = leftId;
    hierarchy.nodes[id].children[1] = rightId;
    return id;
}

} // namespace

size_t PointCloudHierarchy::Bytes () const
{
    size_t bytes = nodes.capacity () * sizeof (PointCloudHierarchyNode);
    for (const PointCloudHierarchyNode& node : nodes)
        bytes += node.Bytes ();
    return bytes;
}

bool BuildPointCloudHierarchy (const PointCloudData& cloud, size_t leafCapacity, size_t representativeCapacity,
                               PointCloudHierarchy& hierarchy, std::string& error)
{
    hierarchy = {};
    error.clear ();
    if (leafCapacity == 0 || representativeCapacity == 0) {
        error = "Point-cloud hierarchy capacities must be greater than zero.";
        return false;
    }
    if (cloud.vertices.size () > std::numeric_limits<uint32_t>::max ()) {
        error = "Point cloud exceeds the hierarchy's 32-bit point-index limit.";
        return false;
    }
    if (cloud.vertices.empty ())
        return true;

    std::vector<uint32_t> indices (cloud.vertices.size ());
    std::iota (indices.begin (), indices.end (), 0u);
    BuildNode (cloud, std::move (indices), leafCapacity, representativeCapacity, 0, hierarchy);
    return true;
}

} // namespace archviz
} // namespace geomsrv
