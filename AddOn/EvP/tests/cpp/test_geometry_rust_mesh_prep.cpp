#include "Geometry/GeometryRustMeshPrep.hpp"
#include "Geometry/VertexWeld.hpp"
#include "Geometry/TriangleDegeneracy.hpp"
#include "Geometry/WireframeEdges.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace rust = tapioca::geometry;
namespace {
void CompareAnalysis (const geomsrv::Mesh& mesh, const std::vector<uint64_t>& keys = {},
                      const std::vector<rust::EdgeRange>& ranges = {})
{
    rust::MeshAnalysis result;
    std::string error;
    ASSERT_TRUE (rust::AnalyzeMesh ({ mesh.vertices, mesh.normals, mesh.triangles, keys, ranges }, result, error))
        << error;
    EXPECT_TRUE (error.empty ());
    EXPECT_EQ (result.normalsUsable, geomsrv::NormalsAreUsable (mesh));
    geomsrv::Mesh smooth = mesh;
    geomsrv::ComputeSmoothNormals (smooth);
    EXPECT_EQ (result.smoothNormals, smooth.normals);
    geomsrv::Aabb bounds;
    for (size_t i = 0; i < mesh.vertices.size (); i += 3)
        bounds.Expand (mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2]);
    for (size_t axis = 0; axis < 3; ++axis) {
        EXPECT_DOUBLE_EQ (result.bounds.mn[axis], bounds.mn[axis]);
        EXPECT_DOUBLE_EQ (result.bounds.mx[axis], bounds.mx[axis]);
        EXPECT_EQ (std::signbit (result.bounds.mn[axis]), std::signbit (bounds.mn[axis]));
        EXPECT_EQ (std::signbit (result.bounds.mx[axis]), std::signbit (bounds.mx[axis]));
    }
    ASSERT_EQ (result.roundoffZero.size (), mesh.TriangleCount ());
    ASSERT_EQ (result.wireMasks.size (), mesh.TriangleCount ());
    for (size_t i = 0; i < mesh.TriangleCount (); ++i) {
        const auto* triangle = &mesh.triangles[i * 3];
        EXPECT_EQ (result.roundoffZero[i],
                   geomsrv::IsRoundoffZeroTriangle (&mesh.vertices[triangle[0] * 3], &mesh.vertices[triangle[1] * 3],
                                                    &mesh.vertices[triangle[2] * 3]))
            << "face " << i;
        std::vector<uint64_t> visible = keys;
        if (!ranges.empty ())
            visible.assign (keys.begin () + ranges[i].offset, keys.begin () + ranges[i].offset + ranges[i].count);
        EXPECT_EQ (result.wireMasks[i], geomsrv::BuildTriangleWireEdgeMask (triangle, visible));
    }
}

void CompareWeld (const std::vector<rust::WeldCorner>& corners, const std::vector<uint64_t>& offsets)
{
    geomsrv::Mesh mesh;
    geomsrv::VertexWelder oracle (mesh);
    std::vector<uint32_t> remap;
    for (size_t body = 1; body < offsets.size (); ++body) {
        oracle.Reset ();
        for (size_t index = offsets[body - 1]; index < offsets[body]; ++index) {
            const auto& c = corners[index];
            remap.push_back (oracle.Add (c.sourceVertex, c.position[0], c.position[1], c.position[2], c.normal[0],
                                         c.normal[1], c.normal[2]));
        }
    }
    rust::WeldedCorners result;
    std::string error;
    ASSERT_TRUE (rust::WeldCorners (corners, offsets, result, error)) << error;
    EXPECT_EQ (result.positions, mesh.vertices);
    EXPECT_EQ (result.normals, mesh.normals);
    EXPECT_EQ (result.cornerVertices, remap);
    // All input contributors survive; first-encounter indices are contiguous.
    std::vector<bool> seen (result.positions.size () / 3, false);
    ASSERT_EQ (result.cornerVertices.size (), corners.size ());
    for (auto vertex : result.cornerVertices) {
        ASSERT_LT (vertex, seen.size ());
        seen[vertex] = true;
    }
    EXPECT_TRUE (std::all_of (seen.begin (), seen.end (), [] (bool value) { return value; }));
}
} // namespace

TEST (GeometryRustMeshPrep, EmptyUnusedDegenerateSurveyAndSignedZero)
{
    CompareAnalysis ({});
    geomsrv::Mesh mesh;
    mesh.vertices = { -0.0, 0, 0, 1, 0, 0, 0, 1, 0, -100, 200, -3 };
    mesh.triangles = { 0, 1, 2, 0, 0, 0, 2, 1, 0 };
    CompareAnalysis (mesh);
    mesh.normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    CompareAnalysis (mesh);
    for (size_t i = 0; i < mesh.vertices.size (); i += 3) {
        mesh.vertices[i] += 6000000;
        mesh.vertices[i + 1] -= 4000000;
    }
    CompareAnalysis (mesh);
    // Source fixture: test_triangledegeneracy.cpp captured native collinear fan.
    mesh.vertices = { 17.991956492089475, 13.040286674170414, 3.0,
                      17.691956492089474, 13.040286674170414, 3.0000000000000004,
                      13.171426334379099, 13.040286674170414, 3.0 };
    mesh.triangles = { 0, 1, 2 };
    mesh.normals.clear ();
    CompareAnalysis (mesh);
}

TEST (GeometryRustMeshPrep, WireMasksMatchAllBitsAndPolygonLocalVisibility)
{
    geomsrv::Mesh mesh;
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh.triangles = { 0, 1, 2 };
    const uint64_t edges[] = { geomsrv::MakeWireEdgeKey (1, 2), geomsrv::MakeWireEdgeKey (2, 0),
                               geomsrv::MakeWireEdgeKey (0, 1) };
    for (unsigned bits = 0; bits < 8; ++bits) {
        std::vector<uint64_t> keys;
        for (unsigned bit = 0; bit < 3; ++bit)
            if (bits & (1u << bit))
                keys.push_back (edges[bit]);
        std::sort (keys.begin (), keys.end ());
        CompareAnalysis (mesh, keys);
    }
    mesh.triangles = { 0, 1, 2, 2, 1, 0 };
    CompareAnalysis (mesh, { edges[0], edges[2] }, { { 0, 1 }, { 1, 1 } });
}

TEST (GeometryRustMeshPrep, SeededMeshesPinFloatAccumulationAndDeterminism)
{
    std::mt19937 generator (0x613203u);
    std::uniform_real_distribution<double> coordinate (-100, 100);
    for (unsigned trial = 0; trial < 80; ++trial) {
        geomsrv::Mesh mesh;
        for (unsigned i = 0; i < 48 * 3; ++i)
            mesh.vertices.push_back (coordinate (generator));
        for (unsigned i = 0; i < 128 * 3; ++i)
            mesh.triangles.push_back (generator () % 48);
        CompareAnalysis (mesh);
        CompareAnalysis (mesh);
    }
}

TEST (GeometryRustMeshPrep, RoundoffBoundaryMatchesCompiledThreeComponentHypot)
{
    const double threshold = 8 * std::numeric_limits<double>::epsilon ();
    for (double height :
         { std::nextafter (threshold, 0.0), threshold, std::nextafter (threshold, 1.0), 1e-10, 1e-200 }) {
        geomsrv::Mesh mesh;
        mesh.vertices = { 0, 0, 0, 1, 0, 0, 1, height, 0 };
        mesh.triangles = { 0, 1, 2 };
        CompareAnalysis (mesh);
    }
    std::mt19937 generator (0x172009u);
    std::uniform_real_distribution<double> component (-1, 1);
    for (unsigned trial = 0; trial < 4000; ++trial) {
        const double x = component (generator), y = component (generator), z = component (generator);
        const double factor = 1 + component (generator) * 1e-14;
        const double thin = 8 * std::numeric_limits<double>::epsilon () * (0.5 + std::abs (component (generator)));
        const double scale = trial % 2 ? 1e-8 : 1e8;
        geomsrv::Mesh mesh;
        mesh.vertices = { 0,
                          0,
                          0,
                          x * scale,
                          y * scale,
                          z * scale,
                          (x * factor + thin) * scale,
                          (y * factor - thin) * scale,
                          z * factor * scale };
        mesh.triangles = { 0, 1, 2 };
        CompareAnalysis (mesh);
    }
}

TEST (GeometryRustMeshPrep, HardCubeCornersSplitButSmoothFallbackRemainsLegacy)
{
    // Same independent unit-cube fixture as test_normals.cpp: 8 shared corners,
    // six outward faces. Flat corner normals require 24 output vertices, not 8.
    const std::array<std::array<double, 3>, 8> positions {
        { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } }
    };
    const int faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
                              { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    const std::array<std::array<double, 3>, 6> normals {
        { { 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { -1, 0, 0 } }
    };
    std::vector<rust::WeldCorner> corners;
    geomsrv::Mesh smooth;
    for (const auto& position : positions)
        smooth.vertices.insert (smooth.vertices.end (), position.begin (), position.end ());
    for (size_t face = 0; face < 6; ++face) {
        for (int vertex : faces[face])
            corners.push_back ({ vertex, positions[vertex], normals[face] });
        for (int local : { 0, 1, 2, 0, 2, 3 })
            smooth.triangles.push_back (static_cast<uint32_t> (faces[face][local]));
    }
    CompareWeld (corners, { 0, 24 });
    CompareAnalysis (smooth);
    rust::WeldedCorners result;
    std::string error;
    ASSERT_TRUE (rust::WeldCorners (corners, std::array<uint64_t, 2> { 0, 24 }, result, error)) << error;
    EXPECT_EQ (result.positions.size (), 24u * 3);
    for (size_t face = 0; face < 6; ++face)
        for (size_t corner = 0; corner < 4; ++corner)
            for (size_t axis = 0; axis < 3; ++axis)
                EXPECT_EQ (result.normals[(face * 4 + corner) * 3 + axis], normals[face][axis]);
}

TEST (GeometryRustMeshPrep, WeldingBodyResetsCreasesHalfwayKeysAndFirstRetention)
{
    const rust::WeldCorner a { 1, { 6000000.123, -4000000.456, 3 }, { 0, 0, 1 } };
    auto b = a;
    b.position = { 99, 88, 77 };
    b.normal[0] = 0.000049;
    CompareWeld ({ a, b, { 1, a.position, { 1, 0, 0 } }, a }, { 0, 3, 3, 4 });
    CompareWeld ({ { 1, a.position, { 0.00005, 0, 1 } },
                   { 1, a.position, { -0.00005, 0, 1 } },
                   { 2, a.position, { 0.00005, 0, 1 } } },
                 { 0, 3 });
    CompareWeld ({}, { 0 });
    CompareWeld ({ { std::numeric_limits<int32_t>::min (), a.position, { -214748.3648, 0, 0 } },
                   { std::numeric_limits<int32_t>::max (), a.position, { 214748.3647, 0, 0 } } },
                 { 0, 2 });
    std::mt19937 generator (0x7913u);
    std::vector<rust::WeldCorner> corners;
    for (unsigned i = 0; i < 6000; ++i) {
        const int32_t source = static_cast<int32_t> (generator () % 97);
        const double noise = static_cast<double> (generator () % 1000) / 1e8;
        corners.push_back ({ source, { double (i), 0, 3 }, { noise, 0, i % 3 ? 1.0 : -1.0 } });
    }
    CompareWeld (corners, { 0, 2000, 2000, 4000, 6000 });
}

TEST (GeometryRustMeshPrep, RefusalPreservesOwnedResultsAndAllowsInputAliasing)
{
    rust::MeshAnalysis result;
    result.smoothNormals = { 77 };
    result.roundoffZero = { 77 };
    result.wireMasks = { 77 };
    std::string error;
    const std::array<double, 3> invalid { std::numeric_limits<double>::quiet_NaN (), 0, 0 };
    EXPECT_FALSE (rust::AnalyzeMesh ({ invalid, {}, {}, {}, {} }, result, error));
    EXPECT_EQ (result.smoothNormals, (std::vector<float> { 77 }));
    EXPECT_EQ (result.roundoffZero, (std::vector<uint8_t> { 77 }));
    EXPECT_EQ (result.wireMasks, (std::vector<uint8_t> { 77 }));
    rust::WeldedCorners welded { { 77 }, { 88 }, { 99 } };
    const std::array<uint64_t, 1> badOffsets { 1 };
    EXPECT_FALSE (rust::WeldCorners ({}, badOffsets, welded, error));
    EXPECT_EQ (welded.positions, (std::vector<double> { 77 }));
    EXPECT_EQ (welded.normals, (std::vector<float> { 88 }));
    EXPECT_EQ (welded.cornerVertices, (std::vector<uint32_t> { 99 }));
    result.smoothNormals = { 0, 0, 1 };
    const std::array<double, 3> point { 1, 2, 3 };
    EXPECT_TRUE (rust::AnalyzeMesh ({ point, result.smoothNormals, {}, {}, {} }, result, error));
    EXPECT_TRUE (result.normalsUsable);
    EXPECT_EQ (result.smoothNormals, (std::vector<float> { 0, 0, 0 }));
}
