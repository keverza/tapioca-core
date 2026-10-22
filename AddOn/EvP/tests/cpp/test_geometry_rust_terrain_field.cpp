#include "Geometry/GeometryRustTerrain.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <numbers>
#include <vector>

namespace {
using namespace tapioca::geometry;

struct Grid {
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    TerrainMeshView View () const
    {
        return { points, triangles };
    }
};

template <class Height> Grid MakeGrid (uint32_t side, Height height)
{
    Grid grid;
    for (uint32_t row = 0; row < side; ++row)
        for (uint32_t column = 0; column < side; ++column)
            grid.points.insert (grid.points.end (),
                                { double (column), double (row), height (double (column), double (row)) });
    for (uint32_t row = 0; row + 1 < side; ++row)
        for (uint32_t column = 0; column + 1 < side; ++column) {
            const uint32_t a = row * side + column;
            grid.triangles.insert (grid.triangles.end (), { a, a + 1, a + side + 1, a, a + side + 1, a + side });
        }
    return grid;
}

// Exact vertex + face attribution (domains = 5) with predecessor support rows,
// the same complete catalog shape the S3a terrain facade tests use.
ProvenanceRequest Provenance (uint64_t vertices, uint64_t faces)
{
    ProvenanceRequest request;
    request.expectedSession = { 1 };
    const EntityCounts counts { vertices, 0, faces, 0 };
    request.counts = counts;
    request.domains = 5;
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
    for (auto [domain, count] : { std::pair { IdentityDomain::Vertex, vertices }, { IdentityDomain::Face, faces } }) {
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

TEST (GeometryRustTerrainField, SamplesATiltedPlaneExactly)
{
    const Grid grid = MakeGrid (6, [] (double x, double y) { return 0.5 * x + 0.25 * y; });
    const std::array<double, 4> queries { 2.3, 1.7, 9.0, 9.0 };
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    std::string error;
    ASSERT_TRUE (SampleTerrain (grid.View (), queries, 0.25, samples, summary, error)) << error;
    ASSERT_EQ (samples.size (), 2U);
    EXPECT_TRUE (samples[0].onSurface);
    EXPECT_NEAR (samples[0].height, 0.5 * 2.3 + 0.25 * 1.7, 1e-12);
    EXPECT_NEAR (samples[0].gradient[0], 0.5, 1e-12);
    EXPECT_NEAR (samples[0].gradient[1], 0.25, 1e-12);
    EXPECT_EQ (samples[0].method[0], TerrainGradientMethod::Central);
    EXPECT_NEAR (samples[0].slope, std::atan (std::hypot (0.5, 0.25)), 1e-12);
    ASSERT_TRUE (samples[0].aspect.has_value ());
    EXPECT_NEAR (*samples[0].aspect, std::atan2 (-0.5, -0.25) + 2 * std::numbers::pi, 1e-12);
    EXPECT_FALSE (samples[1].onSurface);
    EXPECT_NEAR (summary.area, 25.0, 1e-12);
    EXPECT_EQ (summary.faces, 50U);
    // An odd query pool refuses and leaves the outputs as they were.
    const std::array<double, 3> odd { 1, 1, 1 };
    EXPECT_FALSE (SampleTerrain (grid.View (), odd, 0.25, samples, summary, error));
    EXPECT_EQ (samples.size (), 2U);
}

TEST (GeometryRustTerrainField, EditsMoveHeightsWithOwnersAndKeepTheInputCatalog)
{
    const Grid grid = MakeGrid (7, [] (double, double) { return 0.0; });
    const ProvenanceRequest provenance = Provenance (49, 72);
    const std::array<double, 6> curve { 0, 6, -1, 6, 6, -1 };
    std::array<TerrainEdit, 2> edits {};
    edits[0].target = TerrainEditTarget::Point;
    edits[0].position = { 3, 3, 2 };
    edits[0].width = 1;
    edits[0].mode = TerrainEditMode::Absolute;
    edits[0].effect = TerrainEditEffect::Fill;
    edits[0].blur = 2;
    edits[0].maxDistance = 10;
    edits[1].target = TerrainEditTarget::Curve;
    edits[1].geometry = curve;
    edits[1].mode = TerrainEditMode::Absolute;
    edits[1].effect = TerrainEditEffect::Cut;
    edits[1].blur = 1;
    edits[1].maxDistance = 10;
    const TerrainEditRequest request { grid.View (), edits, {}, {}, {}, std::nullopt, provenance };
    TerrainEditResult result;
    std::string error;
    ASSERT_TRUE (EditTerrain (request, result, error)) << error;
    ASSERT_EQ (result.heights.size (), 49U);
    EXPECT_DOUBLE_EQ (result.heights[3 * 7 + 3], 2.0);
    EXPECT_DOUBLE_EQ (result.heights[3 * 7 + 4], 2.0); // inside the flat radius
    EXPECT_DOUBLE_EQ (result.heights[6 * 7 + 3], -1.0);
    EXPECT_EQ (result.owners[3 * 7 + 3], std::optional<uint32_t> (0));
    EXPECT_EQ (result.owners[6 * 7 + 3], std::optional<uint32_t> (1));
    EXPECT_EQ (result.owners[0], std::optional<uint32_t> ());
    ASSERT_EQ (result.reports.size (), 2U);
    EXPECT_GT (result.reports[0].moved, 0U);
    EXPECT_FALSE (result.faces.empty ());
    for (size_t row = 1; row < result.faces.size (); ++row)
        EXPECT_TRUE (result.faces[row - 1].face < result.faces[row].face ||
                     (result.faces[row - 1].face == result.faces[row].face &&
                      result.faces[row - 1].edit < result.faces[row].edit));
    EXPECT_EQ (result.inputProvenance, provenance);
    // Wrong attribution domains: refused, nothing published.
    ProvenanceRequest wrong = provenance;
    wrong.domains = 1;
    const TerrainEditRequest refused { grid.View (), edits, {}, {}, {}, std::nullopt, wrong };
    TerrainEditResult untouched;
    EXPECT_FALSE (EditTerrain (refused, untouched, error));
    EXPECT_TRUE (untouched.heights.empty ());
}

TEST (GeometryRustTerrainField, VolumesReconcilePerEditWithTheEditedSurface)
{
    const Grid grid = MakeGrid (9, [] (double x, double) { return 0.1 * x; });
    const ProvenanceRequest provenance = Provenance (81, 128);
    std::array<TerrainEdit, 2> edits {};
    edits[0].position = { 2, 4, 1.5 };
    edits[0].mode = TerrainEditMode::Relative;
    edits[0].blur = 2;
    edits[0].maxDistance = 10;
    edits[1].position = { 6, 4, -1 };
    edits[1].mode = TerrainEditMode::Relative;
    edits[1].blur = 2;
    edits[1].maxDistance = 10;
    TerrainEditResult edited;
    std::string error;
    ASSERT_TRUE (EditTerrain ({ grid.View (), edits, {}, {}, {}, std::nullopt, provenance }, edited, error)) << error;
    TerrainVolumeRequest request;
    request.existing = grid.View ();
    request.proposed = TerrainVolumeRequest::Proposed::Edited;
    request.heights = edited.heights;
    request.owners = edited.owners;
    request.cell = 0.5;
    request.subdivisions = 2;
    TerrainVolumeReport report;
    ASSERT_TRUE (MeasureTerrainVolumes (request, report, error)) << error;
    ASSERT_EQ (report.edits.size (), 2U);
    EXPECT_GT (report.edits[0].fill, 0.0);
    EXPECT_GT (report.edits[1].cut, 0.0);
    EXPECT_NEAR (report.edits[0].net + report.edits[1].net, report.site.net, 1e-9);
    EXPECT_EQ (report.cells, (std::array<uint32_t, 2> { 16, 16 }));
    // A plane against the existing surface: whole-terrain cut and fill.
    TerrainVolumeRequest plane;
    plane.existing = grid.View ();
    plane.plane = 0.4;
    plane.cell = 1;
    ASSERT_TRUE (MeasureTerrainVolumes (plane, report, error)) << error;
    EXPECT_NEAR (report.site.fill - report.site.cut, report.site.net, 1e-12);
    EXPECT_NEAR (report.site.net, 64 * 0.4 - 0.1 * 4.0 * 64, 1e-9); // mean height 0.4 over 64 cells
    EXPECT_TRUE (report.edits.empty ());
}
} // namespace
