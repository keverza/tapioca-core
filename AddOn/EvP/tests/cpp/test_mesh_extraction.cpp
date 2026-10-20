#include "mesh_extraction_replay.hpp"

#include <cmath>

namespace replay = extraction_replay;

TEST (MeshExtraction, CuratedFanSidecarsCreasesBodyResetsAndRetainedBounds)
{
    const auto result = replay::ReplayCpp (replay::Curated ());
    const auto& mesh = result.mesh;
    ASSERT_TRUE (result.retained);
    EXPECT_EQ (result.counts.degenerateNormals, 2u);
    EXPECT_EQ (result.counts.emptyFanTriangles, 1u);
    EXPECT_EQ (mesh.VertexCount (), 12u);
    EXPECT_EQ (mesh.triangles, (std::vector<uint32_t> { 0, 1, 2, 0, 2, 3, 5, 6, 7, 8, 9, 10, 8, 10, 11 }));
    EXPECT_EQ (mesh.triMaterial, (std::vector<int32_t> { 17, 17, 23, 17, 17 }));
    EXPECT_EQ (mesh.triSurfaceAttribute, (std::vector<int32_t> { 101, 101, 0, 101, 101 }));
    EXPECT_EQ (mesh.triSourceBody, (std::vector<int32_t> { 1, 1, 1, 4, 4 }));
    EXPECT_EQ (mesh.triSourcePgon, (std::vector<int32_t> { 3, 3, 8, 3, 3 }));
    EXPECT_EQ (mesh.triWireEdges, (std::vector<uint8_t> { 5, 3, 4, 5, 3 }));
    EXPECT_DOUBLE_EQ (mesh.bounds.mn[0], -50);
    EXPECT_DOUBLE_EQ (mesh.bounds.mn[2], -9);
    EXPECT_DOUBLE_EQ (mesh.bounds.mx[1], 70);
    EXPECT_TRUE (std::signbit (mesh.vertices[0]));
    EXPECT_EQ (mesh.normals[3 * 5], 1);     // Hard split with same source vertex.
    EXPECT_EQ (mesh.normals[3 * 8 + 2], 1); // Body-local reset.
}

TEST (MeshExtraction, NormalFallbackKeepsLegacyStrictLengthAndFirstThreeRule)
{
    const std::vector<double> positions { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    for (double length : { 0.0, 1e-12, std::nextafter (1e-12, 0.0), std::nextafter (1e-12, 1.0) }) {
        std::vector<geomsrv::ExtractionCorner> corners { { 0, length, 0, 0 }, { 1, 0, 0, 2 }, { 2, 0, 0, 2 } };
        size_t count = 0;
        ASSERT_TRUE (geomsrv::NormalizeExtractionCorners (positions, corners, count));
        EXPECT_EQ (count, length > 1e-12 ? 0u : 1u);
        EXPECT_EQ (corners[0].nx, length > 1e-12 ? 1.0 : 0.0);
        EXPECT_EQ (corners[0].nz, length > 1e-12 ? 0.0 : 1.0);
    }
    auto fixture = replay::Curated ();
    fixture.bodies.resize (1);
    fixture.bodies[0].polygons.resize (1);
    fixture.bodies[0].polygons[0].convexes = { { { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 3, 0, 0, 1 } } };
    const auto result = replay::ReplayCpp (fixture);
    EXPECT_FALSE (result.retained);
    EXPECT_TRUE (result.mesh.vertices.empty ());
    EXPECT_EQ (result.counts.emptyFanTriangles, 0u);
}

TEST (MeshExtraction, AllOmittedFanRetainsFirstVertexAndBoundsButNoTriangleSidecars)
{
    auto fixture = replay::Curated ();
    fixture.bodies.resize (1);
    fixture.bodies[0].polygons.resize (1);
    fixture.bodies[0].polygons[0].convexes.erase (fixture.bodies[0].polygons[0].convexes.begin ());
    const auto result = replay::ReplayCpp (fixture);
    EXPECT_FALSE (result.retained);
    EXPECT_EQ (result.mesh.VertexCount (), 1u);
    EXPECT_EQ (result.counts.emptyFanTriangles, 1u);
    EXPECT_TRUE (result.mesh.triMaterial.empty ());
    EXPECT_TRUE (result.mesh.triSourcePgon.empty ());
    EXPECT_TRUE (result.mesh.triWireEdges.empty ());
    EXPECT_DOUBLE_EQ (result.mesh.bounds.mn[0], -50);
}
