#include "ArchViz/PointCloudLod.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

float DistanceToBounds (const PointCloudHierarchyNode& node, const float eye[3])
{
    float squared = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = std::max (node.boundsMin[axis] - eye[axis], eye[axis] - node.boundsMax[axis]);
        if (delta > 0.0f)
            squared += delta * delta;
    }
    return std::sqrt (squared);
}

float ProjectedError (const PointCloudHierarchyNode& node, const PointCloudLodCamera& camera)
{
    if (camera.orthographic) {
        const float height = std::max (camera.orthographicHeight, 1.0e-6f);
        return node.geometricError * camera.viewportHeightPixels / height;
    }
    const float distance = std::max (DistanceToBounds (node, camera.eye), 1.0e-4f);
    const float tangent = std::max (std::tan (camera.verticalFieldOfViewRadians * 0.5f), 1.0e-6f);
    return node.geometricError * camera.viewportHeightPixels / (2.0f * tangent * distance);
}

size_t ImmediateChildPoints (const PointCloudHierarchy& hierarchy, const PointCloudHierarchyNode& node)
{
    size_t count = 0;
    for (uint32_t child : node.children) {
        if (child < hierarchy.nodes.size ())
            count += hierarchy.nodes[child].pointIndices.size ();
    }
    return count;
}

bool SelectNode (const PointCloudHierarchy& hierarchy, uint32_t id, const PointCloudLodCamera& camera, float pixelError,
                 size_t pointBudget, size_t& selectedPoints, std::vector<uint32_t>& selected)
{
    if (id >= hierarchy.nodes.size ())
        return false;
    const PointCloudHierarchyNode& node = hierarchy.nodes[id];
    const bool refine = !node.IsLeaf () && ProjectedError (node, camera) > pixelError;
    const size_t childPoints = refine ? ImmediateChildPoints (hierarchy, node) : 0;
    if (refine && selectedPoints + childPoints <= pointBudget) {
        const size_t originalCount = selected.size ();
        const size_t originalPoints = selectedPoints;
        const bool left =
            SelectNode (hierarchy, node.children[0], camera, pixelError, pointBudget, selectedPoints, selected);
        const bool right =
            SelectNode (hierarchy, node.children[1], camera, pixelError, pointBudget, selectedPoints, selected);
        if (left && right)
            return true;
        selected.resize (originalCount);
        selectedPoints = originalPoints;
    }

    const size_t points = node.pointIndices.size ();
    if (selected.empty () || selectedPoints + points <= pointBudget) {
        selected.push_back (id);
        selectedPoints += points;
        return true;
    }
    return false;
}

} // namespace

std::vector<uint32_t> SelectPointCloudLod (const PointCloudHierarchy& hierarchy, const PointCloudLodCamera& camera,
                                           float pixelError, size_t pointBudget)
{
    std::vector<uint32_t> selected;
    if (hierarchy.nodes.empty () || pointBudget == 0)
        return selected;
    size_t selectedPoints = 0;
    SelectNode (hierarchy, 0, camera, std::max (pixelError, 0.0f), pointBudget, selectedPoints, selected);
    return selected;
}

} // namespace archviz
} // namespace geomsrv
