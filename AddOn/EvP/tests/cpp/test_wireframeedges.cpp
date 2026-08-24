#include "Geometry/WireframeEdges.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using geomsrv::BuildTriangleWireEdgeMask;
using geomsrv::MakeWireEdgeKey;
using geomsrv::WireEdgeKey;

TEST (WireframeEdges, HidesFanDiagonalAndKeepsFaceBoundary)
{
    std::vector<WireEdgeKey> boundary = {
        MakeWireEdgeKey (0, 1),
        MakeWireEdgeKey (1, 2),
        MakeWireEdgeKey (2, 3),
        MakeWireEdgeKey (3, 0),
    };
    std::sort (boundary.begin (), boundary.end ());

    const uint32_t first[3] = { 0, 1, 2 };
    const uint32_t second[3] = { 0, 2, 3 };
    EXPECT_EQ (BuildTriangleWireEdgeMask (first, boundary), 0x5u);
    EXPECT_EQ (BuildTriangleWireEdgeMask (second, boundary), 0x3u);
}

TEST (WireframeEdges, KeepsOnlyExplicitlyVisibleSourceEdges)
{
    std::vector<WireEdgeKey> visible = { MakeWireEdgeKey (4, 5) };
    const uint32_t triangle[3] = { 4, 5, 6 };
    EXPECT_EQ (BuildTriangleWireEdgeMask (triangle, visible), 0x4u);
}
