#ifndef TAPIOCA_TEST_MESH_EXTRACTION_REPLAY_HPP
#define TAPIOCA_TEST_MESH_EXTRACTION_REPLAY_HPP

#include "Geometry/MeshExtraction.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <array>
#include <random>

namespace extraction_replay {
struct Polygon {
    int32_t index = 1, material = 0, surface = 0;
    std::vector<geomsrv::WireEdgeKey> visibleEdges;
    std::vector<std::vector<geomsrv::ExtractionCorner>> convexes;
};
struct Body {
    int32_t index = 1;
    std::vector<double> positions;
    std::vector<Polygon> polygons;
};
struct Fixture {
    std::string guid = "curated-extraction";
    int32_t elemType = 6;
    std::vector<Body> bodies;
};
struct Result {
    geomsrv::Mesh mesh;
    geomsrv::ExtractionCounts counts;
    bool retained = false;
};

inline std::vector<geomsrv::WireEdgeKey> VisibleEdges (const Polygon& polygon)
{
    auto keys = polygon.visibleEdges;
    std::sort (keys.begin (), keys.end ());
    keys.erase (std::unique (keys.begin (), keys.end ()), keys.end ());
    return keys;
}

inline Result ReplayCpp (const Fixture& fixture)
{
    Result result;
    auto& mesh = result.mesh;
    mesh.guid = fixture.guid;
    mesh.elemType = fixture.elemType;
    geomsrv::VertexWelder welder (mesh);
    for (const auto& body : fixture.bodies) {
        // Same acquisition boundary as GeometryExtractor: a body without either
        // vertices or polygons contributes neither geometry nor retained bounds.
        if (body.positions.empty () || body.polygons.empty ())
            continue;
        for (size_t i = 0; i < body.positions.size (); i += 3)
            mesh.bounds.Expand (body.positions[i], body.positions[i + 1], body.positions[i + 2]);
        welder.Reset ();
        for (const auto& polygon : body.polygons) {
            const auto edges = VisibleEdges (polygon);
            for (auto corners : polygon.convexes)
                geomsrv::AppendExtractedConvex (mesh, welder, body.positions, corners,
                                                { body.index, polygon.index, polygon.material, polygon.surface, edges },
                                                result.counts);
        }
    }
    result.retained = mesh.TriangleCount () != 0;
    if (result.retained && !geomsrv::NormalsAreUsable (mesh))
        geomsrv::ComputeSmoothNormals (mesh);
    return result;
}

inline void ExpectEqual (const Result& actual, const Result& expected)
{
    const auto& a = actual.mesh;
    const auto& b = expected.mesh;
    EXPECT_EQ (actual.retained, expected.retained);
    EXPECT_EQ (actual.counts.degenerateNormals, expected.counts.degenerateNormals);
    EXPECT_EQ (actual.counts.emptyFanTriangles, expected.counts.emptyFanTriangles);
    EXPECT_EQ (a.guid, b.guid);
    EXPECT_EQ (a.elemType, b.elemType);
    ASSERT_EQ (a.vertices.size (), b.vertices.size ());
    for (size_t i = 0; i < a.vertices.size (); ++i)
        EXPECT_EQ (std::bit_cast<uint64_t> (a.vertices[i]), std::bit_cast<uint64_t> (b.vertices[i])) << i;
    ASSERT_EQ (a.normals.size (), b.normals.size ());
    for (size_t i = 0; i < a.normals.size (); ++i)
        EXPECT_EQ (std::bit_cast<uint32_t> (a.normals[i]), std::bit_cast<uint32_t> (b.normals[i])) << i;
    EXPECT_EQ (a.triangles, b.triangles);
    EXPECT_EQ (a.triMaterial, b.triMaterial);
    EXPECT_EQ (a.triSurfaceAttribute, b.triSurfaceAttribute);
    EXPECT_EQ (a.triSourceBody, b.triSourceBody);
    EXPECT_EQ (a.triSourcePgon, b.triSourcePgon);
    EXPECT_EQ (a.triWireEdges, b.triWireEdges);
    for (size_t axis = 0; axis < 3; ++axis) {
        EXPECT_EQ (std::bit_cast<uint64_t> (a.bounds.mn[axis]), std::bit_cast<uint64_t> (b.bounds.mn[axis]));
        EXPECT_EQ (std::bit_cast<uint64_t> (a.bounds.mx[axis]), std::bit_cast<uint64_t> (b.bounds.mx[axis]));
    }
}

inline Fixture Curated ()
{
    // Curated acquisition-shaped data, NOT a recorded SDK body. Only the three
    // collinear coordinates below originate in test_triangledegeneracy.cpp's
    // native capture. IDs, normals, other positions and sidecars are authored.
    Fixture fixture;
    Body body;
    body.positions = { -0.0,
                       0,
                       0,
                       2,
                       0,
                       0,
                       2,
                       2,
                       0,
                       0,
                       2,
                       0,
                       -50,
                       70,
                       -9,
                       17.991956492089475,
                       13.040286674170414,
                       3.0,
                       17.691956492089474,
                       13.040286674170414,
                       3.0000000000000004,
                       13.171426334379099,
                       13.040286674170414,
                       3.0 };
    Polygon polygon;
    polygon.index = 3;
    polygon.material = 17;
    polygon.surface = 101;
    polygon.visibleEdges = { geomsrv::MakeWireEdgeKey (3, 0), geomsrv::MakeWireEdgeKey (1, 0),
                             geomsrv::MakeWireEdgeKey (1, 2), geomsrv::MakeWireEdgeKey (2, 3),
                             geomsrv::MakeWireEdgeKey (1, 0) };
    polygon.convexes = { { { 0, 0, 0, 2 }, { 1, 0, 0, 0 }, { 2, 0, 0, 4 }, { 3, 0, 0, 2 } },
                         // All omitted, but its first welded vertex is retained.
                         { { 5, 0, 0, 1 }, { 6, 0, 0, 1 }, { 7, 0, 0, 1 } },
                         // First three collinear: fallback failure skips the WHOLE
                         // convex even though its last fan piece has positive area.
                         { { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 3, 0, 0, 1 } },
                         { { 0, 0, 0, 1 }, { 900, 0, 0, 1 }, { 1, 0, 0, 1 } },
                         { { 0, 0, 0, 1 }, { 1, 0, 0, 1 } } };
    body.polygons.push_back (polygon);
    polygon.index = 8;
    polygon.material = 23;
    polygon.surface = 0; // Explicit unknown, never inferred from material.
    polygon.visibleEdges = { geomsrv::MakeWireEdgeKey (0, 2) };
    polygon.convexes = { { { 0, 1, 0, 0 }, { 2, 1, 0, 0 }, { 1, 1, 0, 0 } } };
    body.polygons.push_back (polygon);
    fixture.bodies.push_back (body);
    // No polygons => skipped bounds; a later body restarts source indices and
    // weld keys but must preserve global emitted vertex and sidecar order.
    fixture.bodies.push_back ({ 2, { -9999, -9999, -9999 }, {} });
    body.index = 4;
    body.polygons.resize (1);
    body.polygons[0].convexes.resize (1);
    fixture.bodies.push_back (body);
    fixture.bodies.push_back ({ 5, {}, { polygon } });
    return fixture;
}

inline Fixture Seeded (uint32_t seed)
{
    std::mt19937 generator (seed);
    Fixture fixture;
    fixture.guid = "curated-seed-" + std::to_string (seed);
    for (int32_t bodyIndex = 1; bodyIndex <= 3; ++bodyIndex) {
        Body body;
        body.index = bodyIndex;
        const double origin = seed % 2 ? 6000000.0 : 0.0;
        body.positions = { origin, -origin,     0, origin + 2,  -origin,      0,  origin + 3, -origin + 2, 0,
                           origin, -origin + 3, 0, origin - 30, -origin + 40, -20 };
        for (int32_t index = 1; index <= 12; ++index) {
            Polygon polygon;
            polygon.index = index * 2;
            polygon.material = int32_t (generator () % 31);
            polygon.surface = index % 3 ? int32_t (generator () % 100 + 1) : 0;
            for (uint32_t a = 0; a < 4; ++a)
                for (uint32_t b = a + 1; b < 4; ++b)
                    if (generator () % 2)
                        polygon.visibleEdges.push_back (geomsrv::MakeWireEdgeKey (a, b));
            std::vector<geomsrv::ExtractionCorner> corners;
            const int rotation = int (generator () % 4);
            for (int local = 0; local < 4; ++local) {
                const int32_t vertex = (local + rotation) % 4;
                const double z = double (generator () % 8 + 1);
                // Both face fallback and deliberately nongeometric but usable
                // source shading normals; normalization precedes the weld key.
                corners.push_back (
                    { vertex, index % 4 ? 0.0 : z, index % 4 ? 0.0 : z * (vertex + 1) / 8, index % 4 ? z : z / 4 });
            }
            if (index % 5 == 0)
                corners[1] = { corners[1].vertex, 0, 0, 0 };
            if (index % 7 == 0)
                corners.insert (corners.begin () + 1, corners.front ());
            polygon.convexes = { corners, corners };
            body.polygons.push_back (polygon);
        }
        fixture.bodies.push_back (body);
    }
    return fixture;
}
} // namespace extraction_replay
#endif
