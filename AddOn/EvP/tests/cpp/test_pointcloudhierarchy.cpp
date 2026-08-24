#include "ArchViz/PointCloudHierarchy.hpp"

#include <gtest/gtest.h>

#include <set>

using namespace geomsrv::archviz;

namespace {

PointCloudData MakeCloud (size_t count)
{
    PointCloudData cloud;
    for (size_t i = 0; i < count; ++i) {
        PointCloudVertex point;
        point.position[0] = float (i);
        point.position[1] = float ((i * 7) % 5);
        point.position[2] = float ((i * 3) % 2);
        cloud.vertices.push_back (point);
    }
    return cloud;
}

TEST (PointCloudHierarchy, BuildsBoundedLeavesInCoarseFirstOrder)
{
    const PointCloudData cloud = MakeCloud (17);
    PointCloudHierarchy hierarchy;
    std::string error;
    ASSERT_TRUE (BuildPointCloudHierarchy (cloud, 4, 3, hierarchy, error)) << error;
    ASSERT_FALSE (hierarchy.nodes.empty ());
    EXPECT_EQ (hierarchy.nodes[0].id, 0u);
    EXPECT_FALSE (hierarchy.nodes[0].IsLeaf ());
    EXPECT_EQ (hierarchy.nodes[0].pointIndices.size (), 3u);

    std::set<uint32_t> leafPoints;
    for (const PointCloudHierarchyNode& node : hierarchy.nodes) {
        EXPECT_EQ (node.id, &node - hierarchy.nodes.data ());
        if (node.IsLeaf ()) {
            EXPECT_LE (node.pointIndices.size (), 4u);
            leafPoints.insert (node.pointIndices.begin (), node.pointIndices.end ());
        }
        else {
            EXPECT_GT (node.children[0], node.id);
            EXPECT_GT (node.children[1], node.id);
        }
        for (size_t axis = 0; axis < 3; ++axis)
            EXPECT_LE (node.boundsMin[axis], node.boundsMax[axis]);
    }
    EXPECT_EQ (leafPoints.size (), cloud.vertices.size ());
}

TEST (PointCloudHierarchy, IsDeterministicAndTerminatesForDegeneratePoints)
{
    PointCloudData cloud;
    cloud.vertices.resize (20);
    PointCloudHierarchy first;
    PointCloudHierarchy second;
    std::string error;
    ASSERT_TRUE (BuildPointCloudHierarchy (cloud, 2, 2, first, error));
    ASSERT_TRUE (BuildPointCloudHierarchy (cloud, 2, 2, second, error));
    ASSERT_EQ (first.nodes.size (), 1u);
    EXPECT_TRUE (first.nodes[0].IsLeaf ());
    EXPECT_EQ (first.nodes[0].pointIndices, second.nodes[0].pointIndices);
    EXPECT_EQ (first.nodes[0].pointIndices.size (), 20u);
}

TEST (PointCloudHierarchy, ValidatesCapacitiesAndAcceptsEmptyCloud)
{
    PointCloudData cloud;
    PointCloudHierarchy hierarchy;
    std::string error;
    EXPECT_FALSE (BuildPointCloudHierarchy (cloud, 0, 1, hierarchy, error));
    EXPECT_TRUE (BuildPointCloudHierarchy (cloud, 1, 1, hierarchy, error));
    EXPECT_TRUE (hierarchy.nodes.empty ());
}

} // namespace
