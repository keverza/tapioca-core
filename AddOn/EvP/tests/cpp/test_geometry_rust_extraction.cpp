#include "mesh_extraction_replay.hpp"
#include "Geometry/GeometryRustMeshPrep.hpp"

#include <limits>

namespace replay = extraction_replay;
namespace rust = tapioca::geometry;
namespace {
struct PreparedConvex {
    const replay::Body* body;
    const replay::Polygon* polygon;
    size_t convexIndex, firstFan;
    std::vector<geomsrv::ExtractionCorner> corners;
    std::vector<std::vector<size_t>> calls;
};
struct PendingTriangle {
    const replay::Polygon* polygon;
    int32_t body;
    std::array<uint32_t, 3> source;
    std::array<size_t, 3> calls;
};
struct CornerCorrespondence {
    int32_t body, polygon;
    size_t convex, corner;
    uint32_t vertex; // UINT32_MAX means normalized but never emitted.
};
struct RustReplay {
    replay::Result result;
    std::vector<CornerCorrespondence> correspondence;
    std::vector<uint32_t> emittedRemap; // Every Add occurrence, including repeats.
};

// Test-only composition, not another production extractor or public ABI. C++
// retains SDK acquisition and corner normalization authority. Rust classifies
// all candidate fans in one batch, welds the exact legacy Add sequence in one
// batch, then analyzes the emitted mesh in one batch. No per-corner ABI calls.
bool ReplayRust (const replay::Fixture& fixture, RustReplay& output, std::string& error)
{
    RustReplay candidate;
    auto& result = candidate.result;
    auto& mesh = result.mesh;
    mesh.guid = fixture.guid;
    mesh.elemType = fixture.elemType;
    geomsrv::Mesh fans;
    std::vector<PreparedConvex> prepared;
    for (const auto& body : fixture.bodies) {
        if (body.positions.empty () || body.polygons.empty ())
            continue;
        const auto vertexBase = uint32_t (fans.VertexCount ());
        fans.vertices.insert (fans.vertices.end (), body.positions.begin (), body.positions.end ());
        for (size_t i = 0; i < body.positions.size (); i += 3)
            mesh.bounds.Expand (body.positions[i], body.positions[i + 1], body.positions[i + 2]);
        for (const auto& polygon : body.polygons) {
            for (size_t convexIndex = 0; convexIndex < polygon.convexes.size (); ++convexIndex) {
                auto corners = polygon.convexes[convexIndex];
                if (!geomsrv::NormalizeExtractionCorners (body.positions, corners, result.counts.degenerateNormals))
                    continue;
                const size_t firstFan = fans.TriangleCount ();
                for (size_t k = 1; k + 1 < corners.size (); ++k)
                    for (size_t local : { size_t (0), k, k + 1 })
                        fans.triangles.push_back (vertexBase + uint32_t (corners[local].vertex));
                prepared.push_back ({ &body, &polygon, convexIndex, firstFan, std::move (corners), {} });
            }
        }
    }
    rust::MeshAnalysis fanAnalysis;
    if (!rust::AnalyzeMesh ({ fans.vertices, {}, fans.triangles, {}, {} }, fanAnalysis, error))
        return false;

    std::vector<rust::WeldCorner> emitted;
    std::vector<uint64_t> bodyOffsets { 0 };
    std::vector<PendingTriangle> triangles;
    for (const auto& body : fixture.bodies) {
        for (auto& convex : prepared) {
            if (convex.body != &body)
                continue;
            convex.calls.resize (convex.corners.size ());
            const auto emit = [&] (size_t local) {
                const auto& c = convex.corners[local];
                const size_t position = size_t (c.vertex) * 3;
                const size_t call = emitted.size ();
                emitted.push_back (
                    { c.vertex,
                      { body.positions[position], body.positions[position + 1], body.positions[position + 2] },
                      { c.nx, c.ny, c.nz } });
                convex.calls[local].push_back (call);
                return call;
            };
            const auto first = emit (0);
            for (size_t k = 1; k + 1 < convex.corners.size (); ++k) {
                if (fanAnalysis.roundoffZero[convex.firstFan + k - 1]) {
                    ++result.counts.emptyFanTriangles;
                    continue;
                }
                triangles.push_back ({ convex.polygon,
                                       body.index,
                                       { uint32_t (convex.corners[0].vertex), uint32_t (convex.corners[k].vertex),
                                         uint32_t (convex.corners[k + 1].vertex) },
                                       { first, emit (k), emit (k + 1) } });
            }
        }
        bodyOffsets.push_back (emitted.size ());
    }
    rust::WeldedCorners welded;
    if (!rust::WeldCorners (emitted, bodyOffsets, welded, error))
        return false;
    mesh.vertices = std::move (welded.positions);
    mesh.normals = std::move (welded.normals);
    for (const auto& convex : prepared) {
        for (size_t local = 0; local < convex.corners.size (); ++local) {
            const auto& calls = convex.calls[local];
            const uint32_t vertex = calls.empty () ? UINT32_MAX : welded.cornerVertices[calls.front ()];
            for (size_t call : calls)
                EXPECT_EQ (welded.cornerVertices[call], vertex);
            candidate.correspondence.push_back (
                { convex.body->index, convex.polygon->index, convex.convexIndex, local, vertex });
        }
    }
    std::vector<uint64_t> keys;
    std::vector<rust::EdgeRange> ranges;
    for (const auto& triangle : triangles) {
        std::array<uint32_t, 3> vertices;
        for (size_t i = 0; i < 3; ++i) {
            vertices[i] = welded.cornerVertices[triangle.calls[i]];
            mesh.triangles.push_back (vertices[i]);
        }
        mesh.triMaterial.push_back (triangle.polygon->material);
        mesh.triSurfaceAttribute.push_back (triangle.polygon->surface);
        mesh.triSourceBody.push_back (triangle.body);
        mesh.triSourcePgon.push_back (triangle.polygon->index);

        // Only visible edges belonging to THIS source triangle are translated
        // into its welded index domain. Global source keys would light unrelated
        // creases/bodies; one global welded pool would leak polygon visibility.
        const auto sourceKeys = replay::VisibleEdges (*triangle.polygon);
        std::vector<uint64_t> localKeys;
        for (size_t a = 0; a < 3; ++a) {
            const size_t b = (a + 1) % 3;
            if (std::binary_search (sourceKeys.begin (), sourceKeys.end (),
                                    geomsrv::MakeWireEdgeKey (triangle.source[a], triangle.source[b])))
                localKeys.push_back (geomsrv::MakeWireEdgeKey (vertices[a], vertices[b]));
        }
        std::sort (localKeys.begin (), localKeys.end ());
        localKeys.erase (std::unique (localKeys.begin (), localKeys.end ()), localKeys.end ());
        ranges.push_back ({ keys.size (), localKeys.size () });
        keys.insert (keys.end (), localKeys.begin (), localKeys.end ());
    }
    rust::MeshAnalysis analysis;
    if (!rust::AnalyzeMesh ({ mesh.vertices, mesh.normals, mesh.triangles, keys, ranges }, analysis, error))
        return false;
    mesh.triWireEdges = std::move (analysis.wireMasks);
    result.retained = mesh.TriangleCount () != 0;
    if (result.retained && !analysis.normalsUsable)
        mesh.normals = std::move (analysis.smoothNormals);
    // analysis.bounds is intentionally ignored: capture bounds span un-emitted
    // source vertices; replacing them with tight emitted bounds changes culling.
    candidate.emittedRemap = std::move (welded.cornerVertices);
    output = std::move (candidate);
    return true;
}

void Compare (const replay::Fixture& fixture)
{
    const auto expected = replay::ReplayCpp (fixture);
    RustReplay actual, repeated;
    std::string error;
    ASSERT_TRUE (ReplayRust (fixture, actual, error)) << error;
    EXPECT_TRUE (error.empty ());
    replay::ExpectEqual (actual.result, expected);
    ASSERT_TRUE (ReplayRust (fixture, repeated, error)) << error;
    replay::ExpectEqual (repeated.result, actual.result);
    EXPECT_EQ (repeated.emittedRemap, actual.emittedRemap);
    ASSERT_EQ (repeated.correspondence.size (), actual.correspondence.size ());
    for (size_t i = 0; i < actual.correspondence.size (); ++i) {
        const auto& a = actual.correspondence[i];
        const auto& b = repeated.correspondence[i];
        EXPECT_EQ (std::tie (a.body, a.polygon, a.convex, a.corner, a.vertex),
                   std::tie (b.body, b.polygon, b.convex, b.corner, b.vertex));
    }
}
} // namespace

TEST (GeometryRustExtraction, WholeMeshAndCompleteCornerCorrespondence)
{
    const auto fixture = replay::Curated ();
    Compare (fixture);
    RustReplay result;
    std::string error;
    ASSERT_TRUE (ReplayRust (fixture, result, error)) << error;
    EXPECT_EQ (result.emittedRemap, (std::vector<uint32_t> { 0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 11 }));
    ASSERT_EQ (result.correspondence.size (), 14u);
    const std::vector<uint32_t> expected { 0, 1, 2, 3, 4, UINT32_MAX, UINT32_MAX, 5, 6, 7, 8, 9, 10, 11 };
    for (size_t i = 0; i < expected.size (); ++i)
        EXPECT_EQ (result.correspondence[i].vertex, expected[i]) << i;
    EXPECT_EQ (result.correspondence[4].convex, 1u);
    EXPECT_EQ (result.correspondence[7].polygon, 8);
    EXPECT_EQ (result.correspondence[10].body, 4);
    // Replaying borrowed input must not normalize or rewrite source data.
    EXPECT_EQ (fixture.bodies[0].polygons[0].convexes[0][1].nz, 0);
    EXPECT_EQ (fixture.bodies[0].polygons[0].convexes[0][0].nz, 2);
}

TEST (GeometryRustExtraction, PolygonLocalWireVisibilityAfterWeldingAcrossBodiesAndCreases)
{
    for (unsigned mask = 0; mask < 8; ++mask) {
        auto fixture = replay::Curated ();
        for (auto& body : fixture.bodies)
            for (auto& polygon : body.polygons) {
                polygon.convexes = { { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 2, 0, 0, 1 } } };
                polygon.visibleEdges.clear ();
                const uint32_t edges[3][2] = { { 1, 2 }, { 2, 0 }, { 0, 1 } };
                for (unsigned bit = 0; bit < 3; ++bit)
                    if ((polygon.index == 8 ? 7u ^ mask : mask) & (1u << bit))
                        polygon.visibleEdges.push_back (geomsrv::MakeWireEdgeKey (edges[bit][0], edges[bit][1]));
            }
        Compare (fixture);
        const auto expected = replay::ReplayCpp (fixture);
        EXPECT_EQ (expected.mesh.triWireEdges,
                   (std::vector<uint8_t> { uint8_t (mask), uint8_t (7u ^ mask), uint8_t (mask) }));
    }
}

TEST (GeometryRustExtraction, EmptyRejectedAndOmittedConvexesKeepCaptureSemantics)
{
    Compare ({});
    auto fixture = replay::Curated ();
    fixture.bodies.resize (1);
    fixture.bodies[0].polygons.resize (1);
    auto& convexes = fixture.bodies[0].polygons[0].convexes;
    convexes.erase (convexes.begin ());
    Compare (fixture);
    convexes.erase (convexes.begin ());
    Compare (fixture);
    convexes = { { { 0, 0, 0, 1 }, { -1, 0, 0, 1 }, { 1, 0, 0, 1 } } };
    Compare (fixture);
}

TEST (GeometryRustExtraction, SeededWholeMeshesPreserveAllSidecarsAndRepeatExactly)
{
    for (uint32_t seed = 0; seed < 64; ++seed) {
        SCOPED_TRACE (seed);
        Compare (replay::Seeded (0x17503u + seed));
    }
}

TEST (GeometryRustExtraction, RoundoffFanBeforeWeldKeepsTinyAndSurveyPieces)
{
    const double threshold = 8 * std::numeric_limits<double>::epsilon ();
    for (double height :
         { std::nextafter (threshold, 0.0), threshold, std::nextafter (threshold, 1.0), 1e-10, 1e-200 }) {
        replay::Fixture fixture;
        replay::Polygon polygon;
        polygon.convexes = { { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 2, 0, 0, 1 }, { 3, 0, 0, 1 } } };
        fixture.bodies = { { 1, { 0, 0, 0, 1, 0, 0, 1, height, 0, 0, 1, 0 }, { polygon } } };
        Compare (fixture);
        fixture.bodies[0].positions = { 0, 0, 0, 1e-8, 0, 0, 0, 1e-8, 0, 0, 1e-8, 0 };
        Compare (fixture);
    }
}

TEST (GeometryRustExtraction, LegacyNormalOverflowTriggersWholeMeshSmoothFallback)
{
    auto fixture = replay::Curated ();
    for (auto& corner : fixture.bodies[0].polygons[0].convexes[0]) {
        // Preserve the C++ SDK policy: finite components can overflow squared
        // length, divide by infinity to zero, then trigger the final safety net.
        // This is replay of existing behavior, not a new Rust ingress policy.
        corner.nx = 1e308;
        corner.ny = 0;
        corner.nz = 0;
    }
    Compare (fixture);
    const auto expected = replay::ReplayCpp (fixture);
    EXPECT_EQ (expected.mesh.normals[3 * 5], 0);
    EXPECT_EQ (expected.mesh.normals[3 * 5 + 2], -1);
}
