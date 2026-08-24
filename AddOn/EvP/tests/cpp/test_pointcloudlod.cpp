#include "ArchViz/PointCloudLod.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

namespace {

PointCloudHierarchy MakeHierarchy ()
{
    PointCloudHierarchy hierarchy;
    PointCloudHierarchyNode root;
    root.id = 0;
    root.boundsMin[0] = -5.0f;
    root.boundsMax[0] = 5.0f;
    root.boundsMin[1] = root.boundsMin[2] = -1.0f;
    root.boundsMax[1] = root.boundsMax[2] = 1.0f;
    root.geometricError = 10.0f;
    root.children[0] = 1;
    root.children[1] = 2;
    root.pointIndices = { 0, 10 };
    hierarchy.nodes.push_back (root);
    for (uint32_t id = 1; id <= 2; ++id) {
        PointCloudHierarchyNode child;
        child.id = id;
        child.boundsMin[0] = id == 1 ? -5.0f : 0.0f;
        child.boundsMax[0] = id == 1 ? 0.0f : 5.0f;
        child.boundsMin[1] = child.boundsMin[2] = -1.0f;
        child.boundsMax[1] = child.boundsMax[2] = 1.0f;
        child.geometricError = 5.0f;
        child.pointIndices = { id * 10, id * 10 + 1, id * 10 + 2 };
        hierarchy.nodes.push_back (child);
    }
    return hierarchy;
}

TEST (PointCloudLod, PerspectiveRefinesNearAndUsesParentFarAway)
{
    const PointCloudHierarchy hierarchy = MakeHierarchy ();
    PointCloudLodCamera camera;
    camera.viewportHeightPixels = 1000.0f;
    camera.verticalFieldOfViewRadians = 1.0f;
    camera.eye[2] = 10.0f;
    EXPECT_EQ (SelectPointCloudLod (hierarchy, camera, 100.0f, 100), (std::vector<uint32_t> { 1, 2 }));
    camera.eye[2] = 1000.0f;
    EXPECT_EQ (SelectPointCloudLod (hierarchy, camera, 100.0f, 100), (std::vector<uint32_t> { 0 }));
}

TEST (PointCloudLod, OrthographicSelectionDoesNotDependOnEyeDistance)
{
    const PointCloudHierarchy hierarchy = MakeHierarchy ();
    PointCloudLodCamera camera;
    camera.orthographic = true;
    camera.viewportHeightPixels = 1000.0f;
    camera.orthographicHeight = 20.0f;
    const auto nearSelection = SelectPointCloudLod (hierarchy, camera, 100.0f, 100);
    camera.eye[2] = 100000.0f;
    EXPECT_EQ (SelectPointCloudLod (hierarchy, camera, 100.0f, 100), nearSelection);
}

TEST (PointCloudLod, PointBudgetKeepsTheCoarseParent)
{
    const PointCloudHierarchy hierarchy = MakeHierarchy ();
    PointCloudLodCamera camera;
    camera.viewportHeightPixels = 1000.0f;
    camera.eye[2] = 10.0f;
    EXPECT_EQ (SelectPointCloudLod (hierarchy, camera, 1.0f, 5), (std::vector<uint32_t> { 0 }));
    EXPECT_TRUE (SelectPointCloudLod (hierarchy, camera, 1.0f, 0).empty ());
}

} // namespace
