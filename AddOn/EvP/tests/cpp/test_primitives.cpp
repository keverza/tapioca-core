#include "Geometry/Primitives.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

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
