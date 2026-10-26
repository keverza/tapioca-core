#include "Geometry/Primitives.hpp"
#include "Geometry/PlanarTriangulation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
namespace geo = geomsrv::engine;

geo::Vector3 Vertex (const geomsrv::Mesh& mesh, uint32_t index)
{
    const size_t offset = static_cast<size_t> (index) * 3;
    return { mesh.vertices[offset], mesh.vertices[offset + 1], mesh.vertices[offset + 2] };
}

geo::Vector3 Subtract (geo::Vector3 a, geo::Vector3 b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

void ExpectOutwardWinding (const geomsrv::Mesh& mesh)
{
    ASSERT_EQ (mesh.normals.size (), mesh.vertices.size ());
    double signedVolume = 0.0;
    for (size_t index = 0; index < mesh.triangles.size (); index += 3) {
        const uint32_t ia = mesh.triangles[index];
        const uint32_t ib = mesh.triangles[index + 1];
        const uint32_t ic = mesh.triangles[index + 2];
        ASSERT_LT (ia, mesh.VertexCount ());
        ASSERT_LT (ib, mesh.VertexCount ());
        ASSERT_LT (ic, mesh.VertexCount ());
        const auto a = Vertex (mesh, ia), b = Vertex (mesh, ib), c = Vertex (mesh, ic);
        const auto cross = geo::Cross (Subtract (b, a), Subtract (c, a));
        for (uint32_t vertex : { ia, ib, ic }) {
            const size_t offset = static_cast<size_t> (vertex) * 3;
            const geo::Vector3 normal { mesh.normals[offset], mesh.normals[offset + 1], mesh.normals[offset + 2] };
            EXPECT_GT (geo::Dot (cross, normal), 0.0);
        }
        signedVolume += geo::Dot (a, geo::Cross (b, c)) / 6.0;
    }
    EXPECT_NEAR (signedVolume, 21.0, 1.0e-9);
}
} // namespace

TEST (Primitives, ExtrusionWindingMatchesNormalsForBothSweepDirectionsAndOutlineWindings)
{
    std::vector<geo::Vector3> outline { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 1, 0 }, { 1, 1, 0 }, { 1, 4, 0 }, { 0, 4, 0 } };
    for (bool reverse : { false, true }) {
        if (reverse)
            std::reverse (outline.begin (), outline.end ());
        for (double sign : { -1.0, 1.0 }) {
            SCOPED_TRACE (sign);
            SCOPED_TRACE (reverse);
            geomsrv::Mesh mesh;
            std::string error;
            ASSERT_TRUE (geo::MakeExtrusion (outline, { 0, 0, sign * 3 }, mesh, error)) << error;
            ExpectOutwardWinding (mesh);
        }
    }
}

TEST (Primitives, VerticalConcaveProfileHasExactDeterministicCaps)
{
    const std::vector<geo::Vector3> outline { { 0, 0, 0 }, { 0, 4, 0 }, { 0, 4, 1 },
                                              { 0, 1, 1 }, { 0, 1, 4 }, { 0, 0, 4 } };
    for (double sign : { -1.0, 1.0 }) {
        geomsrv::Mesh mesh, repeated;
        std::string error;
        ASSERT_TRUE (geo::MakeExtrusion (outline, { sign * 3, 0, 0 }, mesh, error)) << error;
        ASSERT_TRUE (geo::MakeExtrusion (outline, { sign * 3, 0, 0 }, repeated, error)) << error;
        EXPECT_EQ (mesh.triangles, repeated.triangles);
        EXPECT_EQ (mesh.vertices, repeated.vertices);
        EXPECT_EQ (mesh.normals, repeated.normals);
        EXPECT_EQ (mesh.TriangleCount (), 20u);
        EXPECT_EQ (mesh.VertexCount (), 36u);
        ExpectOutwardWinding (mesh);
        double capArea = 0.0;
        for (size_t index = 0; index < mesh.triangles.size (); index += 3) {
            const auto a = Vertex (mesh, mesh.triangles[index]);
            const auto b = Vertex (mesh, mesh.triangles[index + 1]);
            const auto c = Vertex (mesh, mesh.triangles[index + 2]);
            if (a.x != b.x || a.x != c.x)
                continue;
            capArea += std::abs ((b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y)) * 0.5;
            const double y = (a.y + b.y + c.y) / 3;
            const double z = (a.z + b.z + c.z) / 3;
            EXPECT_TRUE (y <= 1 || z <= 1);
        }
        EXPECT_NEAR (capArea, 14.0, 1.0e-12);
    }
}

TEST (Primitives, CollinearLeadingEdgesAndDuplicateClosurePreserveCapArea)
{
    const std::vector<geo::Vector3> outline { { 0, 0, 0 }, { 2, 0, 0 }, { 4, 0, 0 }, { 4, 1, 0 },
                                              { 1, 1, 0 }, { 1, 4, 0 }, { 0, 4, 0 } };
    auto closed = outline;
    closed.push_back (closed.front ());
    geomsrv::Mesh mesh, repeated, closedMesh;
    std::string error;
    ASSERT_TRUE (geo::MakeExtrusion (outline, { 0, 0, 3 }, mesh, error)) << error;
    ASSERT_TRUE (geo::MakeExtrusion (outline, { 0, 0, 3 }, repeated, error)) << error;
    ASSERT_TRUE (geo::MakeExtrusion (closed, { 0, 0, 3 }, closedMesh, error)) << error;
    EXPECT_EQ (mesh.triangles, repeated.triangles);
    EXPECT_EQ (mesh.triangles, closedMesh.triangles);
    EXPECT_EQ (mesh.vertices, closedMesh.vertices);
    ExpectOutwardWinding (mesh);
    double capArea = 0.0;
    for (size_t index = 0; index < mesh.triangles.size (); index += 3) {
        const auto a = Vertex (mesh, mesh.triangles[index]);
        const auto b = Vertex (mesh, mesh.triangles[index + 1]);
        const auto c = Vertex (mesh, mesh.triangles[index + 2]);
        if (a.z == b.z && a.z == c.z)
            capArea += std::abs ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5;
    }
    EXPECT_NEAR (capArea, 14.0, 1.0e-12);
}

TEST (Primitives, CrossingDegenerateAndNonfiniteOutlinesRefuseWithoutReplacingMesh)
{
    // Nonzero signed area gets past OutlinePlane, so this checks the consumer's
    // repair policy rather than only a zero-area Newell-normal refusal.
    const double crossedXY[] { 0, 0, 4, 4, 4, 0, 0, 3 };
    geo::PlanarTriangulation repaired;
    std::string error;
    ASSERT_TRUE (geo::TriangulatePlanar ({ { crossedXY, 4 } }, false, repaired, error)) << error;
    ASSERT_EQ (repaired.cover, geo::PlanarCover::Repaired);
    const std::vector<std::vector<geo::Vector3>> invalid {
        { { 0, 0, 0 }, { 4, 4, 0 }, { 4, 0, 0 }, { 0, 3, 0 } },
        { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } },
        { { 0, 0, 0 }, { 1, 0, 0 }, { std::numeric_limits<double>::quiet_NaN (), 1, 0 } },
    };
    for (const auto& outline : invalid) {
        geomsrv::Mesh mesh;
        ASSERT_TRUE (geo::MakeBox ({ 1, 2, 3 }, 2, 3, 4, mesh, error)) << error;
        const geomsrv::Mesh before = mesh;
        EXPECT_FALSE (geo::MakeExtrusion (outline, { 0, 0, 3 }, mesh, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_EQ (mesh.vertices, before.vertices);
        EXPECT_EQ (mesh.normals, before.normals);
        EXPECT_EQ (mesh.triangles, before.triangles);
        EXPECT_EQ (mesh.triMaterial, before.triMaterial);
        EXPECT_EQ (mesh.triWireEdges, before.triWireEdges);
    }
}
