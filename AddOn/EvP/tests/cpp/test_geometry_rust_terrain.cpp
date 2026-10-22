#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustIdentityAbi.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using namespace tapioca::geometry;
const std::array<double, 12> kPoints { 0, 0, 0, 10, 0, 0, 10, 10, 8, 0, 10, 0 };
const std::array<uint32_t, 4> kRing { 0, 1, 2, 3 };
ProvenanceRequest Provenance (EntityCounts counts, uint32_t domains)
{
    ProvenanceRequest request;
    request.expectedSession = { 1 };
    request.counts = counts;
    request.domains = domains;
    auto& table = request.table;
    table.context = { 1, request.expectedSession, 0, 100 };
    table.sources = { { 0x20000000000001ULL, {} } };
    table.operations = { { 12, {} }, { 14, {} }, { 15, { 0, 2 } } };
    table.objects = { { 13, 15 } };
    table.sourceIds = { 0x20000000000001ULL };
    table.operationIds = { 14, 12, 15 };
    NativeBinding binding;
    binding.logicalKind = LogicalKind::Object;
    binding.logicalId = 13;
    const std::string guid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
    std::copy (guid.begin (), guid.end (), binding.nativeGuid.begin ());
    binding.metadataPresent = true;
    binding.metadataUuid = { 9 };
    binding.role = { 0, 6 };
    table.bindings = { binding };
    table.roleBytes = { 'o', 'u', 't', 'p', 'u', 't' };
    table.supportTables = { { { 1, request.expectedSession, 0, 90 }, counts, true } };
    for (auto [domain, count] : { std::pair { IdentityDomain::Vertex, counts.vertices },
                                  { IdentityDomain::Edge, counts.edges },
                                  { IdentityDomain::Face, counts.faces } }) {
        for (uint64_t index = 0; index < count; ++index) {
            EntityMapping row;
            row.domain = domain;
            row.kind = MappingKind::Exact;
            row.outputIndex = index;
            row.sources = { 0, 1 };
            row.operations = { 2, 1 };
            row.object = 13;
            row.predecessors = { table.predecessors.size (), 1 };
            table.predecessors.push_back ({ 90, domain, index });
            table.mappings.push_back (row);
        }
    }
    return request;
}
double Area (const TerrainResult& mesh, size_t face)
{
    const auto a = mesh.triangles.at (face * 3) * 3, b = mesh.triangles.at (face * 3 + 1) * 3,
               c = mesh.triangles.at (face * 3 + 2) * 3;
    return .5 * ((mesh.points.at (b) - mesh.points.at (a)) * (mesh.points.at (c + 1) - mesh.points.at (a + 1)) -
                 (mesh.points.at (b + 1) - mesh.points.at (a + 1)) * (mesh.points.at (c) - mesh.points.at (a)));
}
double TotalArea (const TerrainResult& mesh)
{
    double area = 0;
    for (size_t face = 0; face < mesh.triangles.size () / 3; ++face)
        area += Area (mesh, face);
    return area;
}
void CompareRaw (const TerrainRequest& request, const TerrainResult& wrapped)
{
    detail::AbiProvenanceStorage storage;
    TapiocaTerrainRequest input {};
    input.provenance = detail::ConvertProvenanceRequest (request.provenance, storage);
    input.points = { request.points.data (), request.points.size () };
    input.zTolerance = request.zTolerance;
    std::vector<TapiocaTerrainConstraint> constraints;
    std::vector<uint32_t> vertices;
    for (const auto& item : request.constraints) {
        constraints.push_back ({ static_cast<uint32_t> (item.role), static_cast<uint32_t> (vertices.size ()),
                                 static_cast<uint32_t> (item.vertices.size ()) });
        vertices.insert (vertices.end (), item.vertices.begin (), item.vertices.end ());
    }
    input.constraints = { constraints.data (), constraints.size () };
    input.constraintVertices = { vertices.data (), vertices.size () };
    if (request.refinement) {
        const auto& r = *request.refinement;
        input.refinement = { r.minAngleDegrees, r.maxAreaSquareMetres.value_or (0), r.maxAddedVertices, 1,
                             r.maxAreaSquareMetres ? 1U : 0U };
    }
    TapiocaTerrainCounts counts {};
    TapiocaTerrainOutput output {};
    output.counts = &counts;
    output.queryOnly = 1;
    std::array<char, 256> error {};
    ASSERT_EQ (tapioca_terrain_triangulate (&input, &output, error.data (), error.size ()), TAPIOCA_OK)
        << error.data ();
    std::vector<double> points (counts.points);
    std::vector<uint32_t> triangles (counts.triangles);
    std::vector<TapiocaTerrainMapping> mappings (counts.mappings);
    output.points = points.data ();
    output.pointCapacity = points.size ();
    output.triangles = triangles.data ();
    output.triangleCapacity = triangles.size ();
    output.mappings = mappings.data ();
    output.mappingCapacity = mappings.size ();
    output.queryOnly = 0;
    ASSERT_EQ (tapioca_terrain_triangulate (&input, &output, error.data (), error.size ()), TAPIOCA_OK)
        << error.data ();
    EXPECT_EQ (points, wrapped.points);
    EXPECT_EQ (triangles, wrapped.triangles);
    ASSERT_EQ (mappings.size (), wrapped.mappings.size ());
    for (size_t index = 0; index < mappings.size (); ++index) {
        EXPECT_EQ (mappings[index].kind, static_cast<uint32_t> (wrapped.mappings[index].kind));
        EXPECT_EQ (mappings[index].index, wrapped.mappings[index].index);
        EXPECT_TRUE (std::equal (std::begin (mappings[index].values), std::end (mappings[index].values),
                                 wrapped.mappings[index].values.begin ()));
    }
    EXPECT_EQ (wrapped.inputProvenance, request.provenance);
}
} // namespace

TEST (GeometryRustTerrain, AbiLayoutsAndFieldOffsets)
{
    static_assert (sizeof (TapiocaTerrainConstraint) == 12);
    static_assert (sizeof (TapiocaTerrainRefinement) == 32);
    static_assert (sizeof (TapiocaTerrainMapping) == 32);
    static_assert (sizeof (TapiocaTerrainCounts) == 24);
    static_assert (sizeof (TapiocaTerrainOutput) == 64);
    static_assert (sizeof (TapiocaTerrainRequest) == 368);
    static_assert (sizeof (TapiocaTerrainSplitRequest) == 344);
    static_assert (offsetof (TapiocaTerrainRequest, provenance) == 88);
    static_assert (offsetof (TapiocaTerrainSplitRequest, provenance) == 64);
    SUCCEED ();
}
TEST (GeometryRustTerrain, TinAndRefinementRetainTheEntireCatalogAndMatchRawAbi)
{
    auto provenance = Provenance ({ 4, 4, 0, 0 }, 3);
    const std::array constraints { TerrainConstraint { TerrainConstraintRole::Boundary, kRing } };
    TerrainRequest request { kPoints, constraints, 1e-9, {}, provenance };
    TerrainResult result;
    std::string error;
    for (bool refined : { false, true }) {
        request.refinement = refined ? std::optional { TerrainRefinement { 20, 2, 10000 } } : std::nullopt;
        ASSERT_TRUE (TriangulateTerrain (request, result, error)) << error;
        EXPECT_NEAR (TotalArea (result), 100, 1e-9);
        CompareRaw (request, result);
        EXPECT_EQ (result.inputProvenance.table.sourceIds.front (), 0x20000000000001ULL);
        EXPECT_EQ (result.inputPointCount, 4U);
        if (refined)
            EXPECT_GT (result.points.size (), kPoints.size ());
    }
    const auto before = result;
    TerrainRequest self { std::span (result.points).first (12), constraints, 1e-9, {}, result.inputProvenance };
    ASSERT_TRUE (TriangulateTerrain (self, result, error)) << error;
    EXPECT_NEAR (TotalArea (result), 100, 1e-9);
    EXPECT_EQ (result.inputProvenance, before.inputProvenance);
}
TEST (GeometryRustTerrain, HolesCrossingsAndGeneratedSupportsStayAttributable)
{
    std::vector<double> points (kPoints.begin (), kPoints.end ());
    for (size_t i = 2; i < points.size (); i += 3)
        points[i] = 0;
    points.insert (points.end (), { 4, 4, 0, 6, 4, 0, 6, 6, 0, 4, 6, 0, 0, 2, 0, 10, 2, 0, 5, 0, 0, 5, 3, 0 });
    const std::array<uint32_t, 4> hole { 4, 5, 6, 7 };
    const std::array<uint32_t, 2> first { 8, 9 }, second { 10, 11 };
    const std::array constraints { TerrainConstraint { TerrainConstraintRole::Boundary, kRing },
                                   TerrainConstraint { TerrainConstraintRole::Hole, hole },
                                   TerrainConstraint { TerrainConstraintRole::Breakline, first },
                                   TerrainConstraint { TerrainConstraintRole::Breakline, second } };
    auto provenance = Provenance ({ 12, 10, 0, 0 }, 3);
    TerrainRequest request { points, constraints, 1e-9, TerrainRefinement { 0, 2, 10000 }, provenance };
    TerrainResult result;
    std::string error;
    ASSERT_TRUE (TriangulateTerrain (request, result, error)) << error;
    EXPECT_NEAR (TotalArea (result), 96, 1e-8);
    CompareRaw (request, result);
    bool crossing = false;
    for (const auto& mapping : result.mappings) {
        if (mapping.kind == TerrainMappingKind::Crossing) {
            crossing = true;
            EXPECT_LT (mapping.values[4], 10U);
            EXPECT_LT (mapping.values[5], 10U);
        }
        if (mapping.kind == TerrainMappingKind::ConstraintEdge)
            EXPECT_LT (mapping.values[4], 10U);
        if (mapping.kind == TerrainMappingKind::SteinerSegment)
            EXPECT_LT (mapping.values[2], 10U);
    }
    EXPECT_TRUE (crossing);
}
TEST (GeometryRustTerrain, SplitPreservesParentAreasOutsideSurfaceAndPropertyLineMapping)
{
    const std::array<uint32_t, 6> triangles { 0, 1, 2, 0, 2, 3 };
    const std::array<double, 12> line { 2, 2, 0, 8, 2, 0, 8, 8, 0, 2, 8, 0 };
    auto provenance = Provenance ({ 8, 4, 2, 0 }, 7);
    TerrainResult result;
    std::string error;
    ASSERT_TRUE (SplitTerrainMesh ({ kPoints, triangles, line, {}, provenance }, result, error)) << error;
    EXPECT_EQ (result.inputProvenance, provenance);
    EXPECT_TRUE (std::equal (kPoints.begin (), kPoints.end (), result.points.begin ()));
    std::array<double, 2> parentArea {};
    double insideArea = 0;
    size_t lineCount = 0;
    for (const auto& row : result.mappings) {
        if (row.kind == TerrainMappingKind::SplitFace) {
            ASSERT_LT (row.values[0], 2U);
            const auto area = Area (result, row.index);
            parentArea[row.values[0]] += area;
            if (row.values[1])
                insideArea += area;
        }
        if (row.kind == TerrainMappingKind::LineVertex)
            ++lineCount;
    }
    EXPECT_NEAR (insideArea, 36, 1e-9);
    EXPECT_NEAR (parentArea[0], 50, 1e-9);
    EXPECT_NEAR (parentArea[1], 50, 1e-9);
    EXPECT_EQ (lineCount, 4U);
    for (size_t i = 0; i < result.points.size (); i += 3)
        EXPECT_NEAR (result.points[i + 2], .8 * std::min (result.points[i], result.points[i + 1]), 1e-9);
}
TEST (GeometryRustTerrain, NamedRefusalsNeverChangeAnOwnedResult)
{
    auto provenance = Provenance ({ 4, 4, 0, 0 }, 3);
    const std::array constraints { TerrainConstraint { TerrainConstraintRole::Boundary, kRing } };
    TerrainResult output;
    output.points = { 77, 88, 99 };
    output.inputProvenance.expectedRevision = 42;
    const auto before = output;
    std::string error;
    for (int kind = 0; kind < 5; ++kind) {
        auto invalid = provenance;
        auto points = kPoints;
        TerrainRequest bad { points, constraints, 1e-9, {}, invalid };
        if (kind == 0)
            invalid.table.mappings.clear ();
        if (kind == 1)
            invalid.expectedRevision = 1;
        if (kind == 2)
            invalid.counts.edges = 3;
        if (kind == 3)
            points[0] = std::numeric_limits<double>::quiet_NaN ();
        if (kind == 4)
            bad.refinement = TerrainRefinement { 20, 1, 0 };
        EXPECT_FALSE (TriangulateTerrain (bad, output, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_EQ (output, before);
    }
    auto splitProvenance = Provenance ({ 8, 4, 2, 0 }, 7);
    const std::array<uint32_t, 6> triangles { 0, 1, 2, 0, 2, 3 };
    const std::array<double, 4> arcs { .1, 0, 0, 0 };
    EXPECT_FALSE (SplitTerrainMesh ({ kPoints, triangles, kPoints, arcs, splitProvenance }, output, error));
    EXPECT_NE (error.find ("PropertyLineArcUnsupported"), std::string::npos);
    EXPECT_EQ (output, before);
}
TEST (GeometryRustTerrain, SeededSurveyTinConversionIsDeterministic)
{
    uint32_t state = 0x5a2026;
    for (int seed = 0; seed < 24; ++seed) {
        auto random = [&] () {
            state = state * 1664525U + 1013904223U;
            return double (state) / 4294967296.0;
        };
        std::vector<double> points;
        for (size_t i = 0; i < kPoints.size (); i += 3)
            points.insert (points.end (), { 500000 + kPoints[i], 6000000 + kPoints[i + 1], kPoints[i + 2] });
        for (int i = 0; i < 6; ++i)
            points.insert (points.end (), { 500001 + random () * 8, 6000001 + random () * 8, random () * 10 });
        const std::array constraints { TerrainConstraint { TerrainConstraintRole::Boundary, kRing } };
        auto provenance = Provenance ({ points.size () / 3, 4, 0, 0 }, 3);
        TerrainRequest request { points, constraints, 1e-9, TerrainRefinement { 0, 5, 10000 }, provenance };
        TerrainResult first, second;
        std::string error;
        ASSERT_TRUE (TriangulateTerrain (request, first, error)) << seed << ": " << error;
        ASSERT_TRUE (TriangulateTerrain (request, second, error)) << error;
        EXPECT_EQ (first, second);
        EXPECT_NEAR (TotalArea (first), 100, 1e-7);
        CompareRaw (request, first);
    }
}
