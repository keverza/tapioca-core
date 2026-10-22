#include "terrain_fixture.hpp"
#include "Geometry/GeometryRustIdentityAbi.hpp"
#include <gtest/gtest.h>

namespace {
using namespace tapioca::terrain_fixture;
template <class Pod, class Function> TerrainResult RawResult (const Pod& input, Function function)
{
    TapiocaTerrainCounts counts {};
    TapiocaTerrainOutput output {};
    output.counts = &counts;
    output.queryOnly = 1;
    std::array<char, 256> error {};
    Require (function (&input, &output, error.data (), error.size ()) == TAPIOCA_OK, error.data ());
    TerrainResult result;
    result.points.resize (counts.points);
    result.triangles.resize (counts.triangles);
    std::vector<TapiocaTerrainMapping> rows (counts.mappings);
    output.points = result.points.data ();
    output.pointCapacity = result.points.size ();
    output.triangles = result.triangles.data ();
    output.triangleCapacity = result.triangles.size ();
    output.mappings = rows.data ();
    output.mappingCapacity = rows.size ();
    output.queryOnly = 0;
    Require (function (&input, &output, error.data (), error.size ()) == TAPIOCA_OK, error.data ());
    for (const auto& row : rows) {
        TerrainMapping mapping { static_cast<TerrainMappingKind> (row.kind), row.index, {} };
        std::copy (std::begin (row.values), std::end (row.values), mapping.values.begin ());
        result.mappings.push_back (mapping);
    }
    return result;
}
TerrainResult RawTin (const TerrainRequest& request)
{
    detail::AbiProvenanceStorage storage;
    TapiocaTerrainRequest input {};
    input.provenance = detail::ConvertProvenanceRequest (request.provenance, storage);
    input.points = { request.points.data (), request.points.size () };
    input.zTolerance = request.zTolerance;
    std::vector<TapiocaTerrainConstraint> constraints;
    std::vector<uint32_t> indices;
    for (const auto& c : request.constraints) {
        constraints.push_back ({ static_cast<uint32_t> (c.role), static_cast<uint32_t> (indices.size ()),
                                 static_cast<uint32_t> (c.vertices.size ()) });
        indices.insert (indices.end (), c.vertices.begin (), c.vertices.end ());
    }
    input.constraints = { constraints.data (), constraints.size () };
    input.constraintVertices = { indices.data (), indices.size () };
    if (request.refinement)
        input.refinement = { request.refinement->minAngleDegrees, request.refinement->maxAreaSquareMetres.value_or (0),
                             request.refinement->maxAddedVertices, 1, 1 };
    auto result = RawResult (input, tapioca_terrain_triangulate);
    result.inputProvenance = request.provenance;
    result.inputPointCount = request.points.size () / 3;
    return result;
}
TerrainResult RawSplit (const TerrainSplitRequest& request)
{
    detail::AbiProvenanceStorage storage;
    TapiocaTerrainSplitRequest input {};
    input.provenance = detail::ConvertProvenanceRequest (request.provenance, storage);
    input.points = { request.points.data (), request.points.size () };
    input.triangles = { request.triangles.data (), request.triangles.size () };
    input.line = { request.line.data (), request.line.size () };
    input.arcAngles = { request.arcAngles.data (), request.arcAngles.size () };
    auto result = RawResult (input, tapioca_terrain_split_mesh);
    result.inputProvenance = request.provenance;
    result.inputPointCount = request.points.size () / 3;
    return result;
}
TEST (GeometryRustTerrainCorpus, SharedLandscapePathsSurfacesAndSplitAncestry)
{
    for (const auto& input : Corpus ()) {
        SCOPED_TRACE (input.name);
        const auto constraints = input.Constraints ();
        const auto provenance = Provenance ({ input.points.size () / 3, input.Segments (), 0, 0 }, 3);
        const auto request = Request (input, constraints, provenance);
        TerrainResult baseline, repeat;
        std::string error;
        ASSERT_TRUE (TriangulateTerrain (request, baseline, error)) << error;
        EXPECT_NO_THROW (CheckTin (input, baseline));
        EXPECT_EQ (baseline, RawTin (request));
        ASSERT_TRUE (TriangulateTerrain (request, repeat, error)) << error;
        EXPECT_EQ (baseline, repeat);
        const auto splitProvenance = Provenance ({ baseline.points.size () / 3 + input.line.size () / 3,
                                                   input.line.size () / 3, baseline.triangles.size () / 3, 0 },
                                                 7);
        const TerrainSplitRequest splitRequest { baseline.points, baseline.triangles, input.line, {}, splitProvenance };
        TerrainResult split;
        ASSERT_TRUE (SplitTerrainMesh (splitRequest, split, error)) << error;
        EXPECT_EQ (split.inputProvenance, splitProvenance);
        EXPECT_EQ (split, RawSplit (splitRequest));
        EXPECT_NO_THROW (CheckSplit (input, baseline, split));
        ASSERT_TRUE (SplitTerrainMesh (splitRequest, repeat, error)) << error;
        EXPECT_EQ (split, repeat);
        // Falsification checks: ensure these geometric assertions are not merely
        // facade-vs-same-ABI comparisons or self-fulfilling expected dumps.
        auto sawtooth = baseline;
        sawtooth.points.at (2) += 1;
        EXPECT_THROW (CheckTin (input, sawtooth), std::runtime_error);
        auto surfaceDrift = split;
        surfaceDrift.points.at (baseline.points.size () + 2) += 1;
        EXPECT_THROW (CheckSplit (input, baseline, surfaceDrift), std::runtime_error);
    }
}
} // namespace
