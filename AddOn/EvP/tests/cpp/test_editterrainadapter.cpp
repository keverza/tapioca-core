#include "Edit/EditTerrainAdapter.hpp"
#include "Edit/EditDocument.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using namespace tapioca::geometry;
namespace edit = geomsrv::edit;
constexpr OperationId kFirst = 0x20000000000001ULL, kSecond = kFirst + 1;

struct TerrainFixture {
    edit::Document doc;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    ProvenanceRequest provenance;
    std::vector<double> property { -1, -1, 0, 7, -1, 0, 7, 7, 0, -1, 7, 0 };

    TerrainFixture ()
    {
        doc.revision = 7;
        doc.landscape.emplace ();
        doc.landscape->sessionUuid = { 1 };
        doc.landscape->sourceTerrainGuid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
        for (OperationId id : { kFirst, kSecond }) {
            LandscapeOperation operation;
            operation.id = id;
            operation.kind = OperationKind::RegionSelect;
            doc.landscape->operations.push_back (operation);
        }
        for (uint32_t y = 0; y < 7; ++y)
            for (uint32_t x = 0; x < 7; ++x)
                points.insert (points.end (), { double (x), double (y), 0 });
        for (uint32_t y = 0; y < 6; ++y)
            for (uint32_t x = 0; x < 6; ++x) {
                const uint32_t a = y * 7 + x;
                triangles.insert (triangles.end (), { a, a + 1, a + 8, a, a + 8, a + 7 });
            }
        provenance.expectedSession = doc.landscape->sessionUuid;
        provenance.expectedRevision = doc.revision;
        provenance.counts = { 49, 0, 72, 0 };
        auto& table = provenance.table;
        table.context = { 1, provenance.expectedSession, doc.revision, 100 };
        table.sources = { { 10, {} } };
        table.operations = { { kFirst, {} }, { kSecond, {} } };
        table.objects = { { 13, kFirst } };
        table.sourceIds = { 10 };
        table.operationIds = { kFirst };
        table.supportTables = { { { 1, provenance.expectedSession, 6, 90 }, provenance.counts, true } };
        NativeBinding binding;
        binding.logicalKind = LogicalKind::Object;
        binding.logicalId = 13;
        std::copy (doc.landscape->sourceTerrainGuid.begin (), doc.landscape->sourceTerrainGuid.end (),
                   binding.nativeGuid.begin ());
        binding.metadataPresent = true;
        binding.metadataUuid = { 9 };
        binding.role = { 0, 6 };
        table.roleBytes = { 'o', 'u', 't', 'p', 'u', 't' };
        table.bindings = { binding };
        for (auto [domain, count] :
             { std::pair { IdentityDomain::Vertex, 49U }, std::pair { IdentityDomain::Face, 72U } })
            for (uint32_t i = 0; i < count; ++i) {
                EntityMapping mapping;
                mapping.domain = domain;
                mapping.kind = MappingKind::Exact;
                mapping.outputIndex = i;
                mapping.sources = { 0, 1 };
                mapping.operations = { 0, 1 };
                mapping.object = 13;
                mapping.predecessors = { table.predecessors.size (), 1 };
                table.predecessors.push_back ({ 90, domain, i });
                table.mappings.push_back (mapping);
            }
    }
    EditTerrainInput Input () const
    {
        return { { points, triangles }, provenance, doc.revision, doc.landscape->sourceTerrainGuid };
    }
    TerrainEdit Area () const
    {
        TerrainEdit area;
        area.target = TerrainEditTarget::Area;
        area.geometry = property;
        area.level = 2;
        area.blur = 1;
        area.maxDistance = 10;
        return area;
    }
};

EditTerrainResult Sentinel ()
{
    EditTerrainResult result;
    result.revision = 999;
    result.sourceTerrainGuid = "unchanged";
    result.points = { 123 };
    result.triangles = { 456 };
    result.vertexOperations = { kSecond };
    result.reports = { { kSecond, { 1, 2, 3 } } };
    result.volumes.emplace ();
    result.volumes->site.fill = 789;
    return result;
}
void ExpectSentinel (const EditTerrainResult& result)
{
    EXPECT_EQ (result.revision, 999U);
    EXPECT_EQ (result.sourceTerrainGuid, "unchanged");
    EXPECT_EQ (result.points, (std::vector<double> { 123 }));
    EXPECT_EQ (result.triangles, (std::vector<uint32_t> { 456 }));
    EXPECT_EQ (result.vertexOperations, (std::vector<std::optional<OperationId>> { kSecond }));
    EXPECT_EQ (result.reports, (std::vector<EditTerrainOperationReport> { { kSecond, { 1, 2, 3 } } }));
    ASSERT_TRUE (result.volumes);
    EXPECT_EQ (result.volumes->site.fill, 789);
}

TEST (EditTerrainAdapter, SamplesTiltedBaselineWithoutChangingDocumentOrCapture)
{
    TerrainFixture fixture;
    for (size_t i = 0; i < fixture.points.size (); i += 3)
        fixture.points[i + 2] = 0.5 * fixture.points[i] + 0.25 * fixture.points[i + 1];
    const auto original = fixture.points;
    const auto landscape = fixture.doc.landscape;
    const auto input = fixture.Input ();
    const std::array<double, 4> queries { 2.3, 1.7, 9, 9 };
    EditTerrainRequest request { input };
    request.queriesXY = queries;
    EditTerrainResult result;
    std::string error = "old error";
    ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_TRUE (error.empty ());
    EXPECT_EQ (result.revision, fixture.doc.revision);
    EXPECT_EQ (result.sourceTerrainGuid, input.sourceTerrainGuid);
    EXPECT_EQ (result.points, original);
    EXPECT_EQ (result.triangles, fixture.triangles);
    EXPECT_EQ (result.inputProvenance, fixture.provenance);
    EXPECT_EQ (fixture.points, original);
    EXPECT_EQ (fixture.doc.landscape, landscape);
    ASSERT_EQ (result.samples.size (), 2U);
    EXPECT_TRUE (result.samples[0].onSurface);
    EXPECT_NEAR (result.samples[0].height, 1.575, 1e-12);
    EXPECT_NEAR (result.samples[0].gradient[0], 0.5, 1e-12);
    EXPECT_NEAR (result.samples[0].gradient[1], 0.25, 1e-12);
    EXPECT_NEAR (result.samples[0].slope, std::atan (std::hypot (0.5, 0.25)), 1e-12);
    EXPECT_TRUE (result.samples[0].aspect.has_value ());
    EXPECT_FALSE (result.samples[1].onSurface);
    EXPECT_NEAR (result.summary.area, 36, 1e-12);
    EXPECT_FALSE (result.volumes);
    EXPECT_TRUE (std::all_of (result.vertexOperations.begin (), result.vertexOperations.end (),
                              [] (const auto& id) { return !id; }));
}

TEST (EditTerrainAdapter, AreaFillHasAnalyticVolumesAndStableOperationOwners)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> operations { { { kFirst, fixture.Area () } } };
    const std::array<double, 2> queries { 3, 3 };
    EditTerrainRequest request { input };
    request.operations = operations;
    request.property = fixture.property;
    request.queriesXY = queries;
    request.volumes.emplace ();
    EditTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (result.inputProvenance, fixture.provenance);
    EXPECT_EQ (result.triangles, fixture.triangles);
    for (size_t i = 0; i < result.vertexOperations.size (); ++i) {
        EXPECT_EQ (result.vertexOperations[i], kFirst);
        EXPECT_EQ (result.points[3 * i], fixture.points[3 * i]);
        EXPECT_EQ (result.points[3 * i + 1], fixture.points[3 * i + 1]);
        EXPECT_EQ (result.points[3 * i + 2], 2);
        EXPECT_EQ (fixture.points[3 * i + 2], 0);
    }
    ASSERT_EQ (result.samples.size (), 1U);
    EXPECT_EQ (result.samples[0].height, 2);
    ASSERT_EQ (result.reports.size (), 1U);
    EXPECT_EQ (result.reports[0].operation, kFirst);
    EXPECT_EQ (result.reports[0].report.moved, 49U);
    ASSERT_EQ (result.faces.size (), 72U);
    for (const auto& face : result.faces)
        EXPECT_EQ (face.operation, kFirst);
    ASSERT_TRUE (result.volumes);
    EXPECT_NEAR (result.volumes->site.fill, 72, 1e-12);
    EXPECT_EQ (result.volumes->site.cut, 0);
    EXPECT_EQ (result.volumes->context.fill, 0);
    EXPECT_EQ (result.volumes->cells, (std::array<uint32_t, 2> { 24, 24 }));
    ASSERT_EQ (result.volumes->operations.size (), 1U);
    EXPECT_EQ (result.volumes->operations[0].operation, kFirst);
    EXPECT_NEAR (result.volumes->operations[0].net, result.volumes->site.net, 1e-12);
}

TEST (EditTerrainAdapter, ForwardsEveryTargetAndSmoothingToTheSingleRustAuthority)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    const std::array<double, 6> line { 0, 3, 2, 6, 3, 2 };
    std::vector<double> surface = fixture.points;
    for (size_t i = 2; i < surface.size (); i += 3)
        surface[i] = 2;
    const std::array<TerrainMeshView, 1> surfaces { { { surface, fixture.triangles } } };
    for (auto target : { TerrainEditTarget::Point, TerrainEditTarget::Curve, TerrainEditTarget::Area,
                         TerrainEditTarget::Path, TerrainEditTarget::Surface }) {
        SCOPED_TRACE (static_cast<uint32_t> (target));
        TerrainEdit edit = fixture.Area ();
        edit.target = target;
        edit.position = { 3, 3, 2 };
        edit.width = 2;
        if (target == TerrainEditTarget::Curve || target == TerrainEditTarget::Path)
            edit.geometry = line;
        const std::array<TerrainEdit, 1> raw { edit };
        const std::array<EditTerrainOperation, 1> operations { { { kFirst, edit } } };
        TerrainEditResult expected;
        std::string error;
        ASSERT_TRUE (
            EditTerrain ({ input.mesh, raw, surfaces, fixture.property, {}, 0.5, fixture.provenance }, expected, error))
            << error;
        EditTerrainRequest request { input };
        request.operations = operations;
        request.property = fixture.property;
        request.surfaces = surfaces;
        request.smoothing = 0.5;
        EditTerrainResult result;
        ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, request, result, error)) << error;
        EXPECT_EQ (result.triangles, fixture.triangles);
        for (size_t i = 0; i < expected.heights.size (); ++i) {
            EXPECT_EQ (result.points[3 * i + 2], expected.heights[i]);
            EXPECT_EQ (result.vertexOperations[i],
                       expected.owners[i] ? std::optional<OperationId> (kFirst) : std::nullopt);
        }
        EXPECT_EQ (result.reports[0].report, expected.reports[0]);
    }
}

TEST (EditTerrainAdapter, PropertyAndProtectedVerticesStayFixedDuringSmoothing)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    const std::array<double, 12> clip { 0.5, 0.5, 0, 5.5, 0.5, 0, 5.5, 5.5, 0, 0.5, 5.5, 0 };
    const std::array<double, 12> protectedRing { 2, 2, 0, 4, 2, 0, 4, 4, 0, 2, 4, 0 };
    const std::array<std::span<const double>, 1> protectedRings { protectedRing };
    const std::array<EditTerrainOperation, 1> operations { { { kFirst, fixture.Area () } } };
    EditTerrainRequest request { input };
    request.operations = operations;
    request.property = clip;
    request.protectedRings = protectedRings;
    request.smoothing = 1;
    request.volumes.emplace ();
    EditTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, request, result, error)) << error;
    for (size_t y = 0; y < 7; ++y)
        for (size_t x = 0; x < 7; ++x)
            if (x == 0 || x == 6 || y == 0 || y == 6 || (x >= 2 && x <= 4 && y >= 2 && y <= 4)) {
                const size_t vertex = y * 7 + x;
                EXPECT_EQ (result.points[vertex * 3 + 2], 0);
                EXPECT_FALSE (result.vertexOperations[vertex]);
            }
    EXPECT_GT (result.reports[0].report.moved, 0U);
    ASSERT_TRUE (result.volumes);
    EXPECT_NEAR (result.volumes->operations[0].net, result.volumes->site.net + result.volumes->context.net, 1e-12);
}

TEST (EditTerrainAdapter, CutAndFillVolumesResolveDistinctNonIndexOperationIds)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    TerrainEdit fill;
    fill.position = { 2, 3, 1 };
    fill.blur = 1;
    fill.maxDistance = 10;
    TerrainEdit cut = fill;
    cut.position = { 4, 3, -1 };
    const std::array<EditTerrainOperation, 2> operations { { { kFirst, fill }, { kSecond, cut } } };
    EditTerrainRequest request { input };
    request.operations = operations;
    request.property = fixture.property;
    request.volumes.emplace ();
    EditTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (result.vertexOperations[3 * 7 + 2], kFirst);
    EXPECT_EQ (result.vertexOperations[3 * 7 + 4], kSecond);
    ASSERT_TRUE (result.volumes);
    ASSERT_EQ (result.volumes->operations.size (), 2U);
    EXPECT_EQ (result.volumes->operations[0].operation, kFirst);
    EXPECT_EQ (result.volumes->operations[1].operation, kSecond);
    EXPECT_GT (result.volumes->operations[0].fill, 0);
    EXPECT_GT (result.volumes->operations[1].cut, 0);
    EXPECT_NEAR (result.volumes->operations[0].net + result.volumes->operations[1].net,
                 result.volumes->site.net + result.volumes->context.net, 1e-12);
}

TEST (EditTerrainAdapter, RefusesStaleSessionRevisionAndBaselineWithoutPublishing)
{
    TerrainFixture fixture;
    auto input = fixture.Input ();
    EditTerrainRequest request { input };
    auto result = Sentinel ();
    std::string error;
    --input.revision;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("ProvenanceStale:"), 0U);
    input.revision = fixture.doc.revision;
    ++fixture.provenance.expectedRevision;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    --fixture.provenance.expectedRevision;
    fixture.provenance.expectedSession = { 2 };
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    fixture.provenance.expectedSession = fixture.doc.landscape->sessionUuid;
    ++fixture.provenance.table.context.revision;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    --fixture.provenance.table.context.revision;
    input.sourceTerrainGuid.clear ();
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("ProvenanceStale:"), 0U);
    input.sourceTerrainGuid = fixture.doc.landscape->sourceTerrainGuid;
    fixture.doc.landscape.reset ();
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    ExpectSentinel (result);
}

TEST (EditTerrainAdapter, RefusesMissingAndMismatchedAttribution)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    EditTerrainRequest request { input };
    auto result = Sentinel ();
    std::string error;
    const auto original = fixture.provenance;
    for (uint32_t domains : { 1U, 3U, 7U }) {
        fixture.provenance.domains = domains;
        EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    }
    fixture.provenance = original;
    --fixture.provenance.counts.vertices;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    fixture.provenance = original;
    fixture.provenance.table.mappings.pop_back ();
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    fixture.provenance = original;
    fixture.provenance.table.bindings[0].nativeGuid[0] = 'x';
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    ExpectSentinel (result);
}

TEST (EditTerrainAdapter, RefusesAbsentDisabledDuplicateAndOutOfOrderOperations)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    std::array<EditTerrainOperation, 2> operations { { { kFirst, fixture.Area () }, { kSecond, fixture.Area () } } };
    EditTerrainRequest request { input };
    request.property = fixture.property;
    request.operations = operations;
    auto result = Sentinel ();
    std::string error;
    for (OperationId invalid : { 0ULL, 99ULL, kSecond }) {
        operations[0].operation = invalid;
        EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    }
    operations[0].operation = kSecond;
    operations[1].operation = kFirst;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    operations[0].operation = kFirst;
    operations[1].operation = kSecond;
    fixture.doc.landscape->operations[0].enabled = false;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    fixture.doc.landscape->operations[0].enabled = true;
    // An enabled document operation with no retained catalog row is not attributable.
    fixture.provenance.table.operations.pop_back ();
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    ExpectSentinel (result);
}

TEST (EditTerrainAdapter, LateSamplingAndVolumeRefusalsKeepTheEntireOutput)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> operations { { { kFirst, fixture.Area () } } };
    EditTerrainRequest request { input };
    request.operations = operations;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("PropertyLineOutside:"), 0U);
    request.property = fixture.property;
    const std::array<double, 3> oddQueries { 1, 1, 1 };
    request.queriesXY = oddQueries;
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    ExpectSentinel (result);
    request.queriesXY = {};
    request.volumes = EditTerrainVolumeOptions { 0, 1 };
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    ExpectSentinel (result);
    request.volumes.reset ();
    fixture.points[2] = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (EvaluateEditTerrain (fixture.doc, request, result, error));
    ExpectSentinel (result);
}

TEST (EditTerrainAdapter, CallerCanBorrowThePreviousOutputUntilAtomicPublication)
{
    TerrainFixture fixture;
    const auto input = fixture.Input ();
    EditTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, { input }, result, error)) << error;
    const EditTerrainInput borrowed {
        { result.points, result.triangles }, result.inputProvenance, result.revision, result.sourceTerrainGuid
    };
    const std::array<double, 2> queries { 3, 3 };
    EditTerrainRequest request { borrowed };
    request.queriesXY = queries;
    ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (result.points, fixture.points);
    EXPECT_EQ (result.inputProvenance, fixture.provenance);
    ASSERT_EQ (result.samples.size (), 1U);
    EXPECT_EQ (result.samples[0].height, 0);
}
} // namespace
