#include "Edit/EditLandscapeTerrain.hpp"
#include "Edit/EditDocument.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace {
using namespace tapioca::geometry;
namespace edit = geomsrv::edit;

struct LandscapeFixture {
    edit::Document doc;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    ProvenanceRequest provenance;
    Id property = 0, footprint = 0;

    LandscapeFixture ()
    {
        std::string error;
        EXPECT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
        property = Ring ({ { 0, 0, 50 }, { 8, 0, 50 }, { 8, 8, 50 }, { 0, 8, 50 } });
        EXPECT_TRUE (SetLandscapePropertyLine (doc, property, error)) << error;
        footprint = Ring ({ { 1, 1, 20 }, { 7, 1, 20 }, { 7, 7, 20 }, { 1, 7, 20 } });
        for (uint32_t y = 0; y < 9; ++y)
            for (uint32_t x = 0; x < 9; ++x)
                points.insert (points.end (), { double (x), double (y), 0.1 * double (x) + 0.2 * double (y) });
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                const uint32_t a = y * 9 + x;
                triangles.insert (triangles.end (), { a, a + 1, a + 10, a, a + 10, a + 9 });
            }
    }
    Id Ring (std::vector<Vec3> ring)
    {
        LandscapeSource source;
        source.entityId = edit::AddPolyline (doc, ring, true);
        Id id = 0;
        std::string error;
        EXPECT_TRUE (AddLandscapeSource (doc, source, id, error)) << error;
        return id;
    }
    Id Square (double x, double y, double size)
    {
        return Ring ({ { x, y, 0 }, { x + size, y, 0 }, { x + size, y + size, 0 }, { x, y + size, 0 } });
    }
    Id Operation (OperationKind kind, std::vector<Id> inputs)
    {
        LandscapeOperation operation;
        operation.kind = kind;
        operation.inputs = std::move (inputs);
        Id id = 0;
        std::string error;
        EXPECT_TRUE (AddLandscapeOperation (doc, operation, id, error)) << error;
        return id;
    }
    Id Select (Id source)
    {
        return Operation (OperationKind::RegionSelect, { source });
    }
    void Capture ()
    {
        provenance = {};
        provenance.expectedSession = doc.landscape->sessionUuid;
        provenance.expectedRevision = doc.revision;
        provenance.counts = { points.size () / 3, 0, triangles.size () / 3, 0 };
        auto& table = provenance.table;
        table.context = { 1, provenance.expectedSession, doc.revision, 100 };
        for (const auto& source : doc.landscape->sources)
            table.sources.push_back ({ source.id, {} });
        // Retain the complete current operation catalog and its ordered parents.
        for (const auto& operation : doc.landscape->operations) {
            IdentityRange parents { table.operationIds.size (), 0 };
            for (Id input : operation.inputs)
                if (std::any_of (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                 [=] (const auto& op) { return op.id == input; })) {
                    table.operationIds.push_back (input);
                    ++parents.count;
                }
            table.operations.push_back ({ operation.id, parents });
        }
        const OperationId operation = doc.landscape->operations.front ().id;
        const size_t mappingOperation = table.operationIds.size ();
        table.operationIds.push_back (operation);
        table.objects = { { doc.nextId + 100,
                            operation } }; // Captured terrain owner, not a document object allocation.
        table.sourceIds = { footprint };
        table.supportTables = { { { 1, provenance.expectedSession, doc.revision, 90 }, provenance.counts, true } };
        for (auto [domain, count] : { std::pair { IdentityDomain::Vertex, provenance.counts.vertices },
                                      std::pair { IdentityDomain::Face, provenance.counts.faces } })
            for (uint64_t i = 0; i < count; ++i) {
                EntityMapping row;
                row.domain = domain;
                row.kind = MappingKind::Exact;
                row.outputIndex = i;
                row.sources = { 0, 1 };
                row.operations = { mappingOperation, 1 };
                row.object = table.objects.front ().id;
                row.predecessors = { table.predecessors.size (), 1 };
                table.predecessors.push_back ({ 90, domain, i });
                table.mappings.push_back (row);
            }
    }
    EditTerrainInput Input () const
    {
        return { { points, triangles }, provenance, doc.revision, doc.landscape->sourceTerrainGuid };
    }
    EditTerrainOperation Area (Id operation) const
    {
        TerrainEdit parameters;
        parameters.target = TerrainEditTarget::Area;
        parameters.level = 5;
        parameters.blur = 1;
        parameters.maxDistance = 10;
        return { operation, parameters };
    }
};

LandscapeTerrainResult Sentinel ()
{
    LandscapeTerrainResult result;
    result.landscape.revision = 999;
    result.landscape.propertyLine = { { 90, 91, 92 } };
    result.terrain.revision = 888;
    result.terrain.points = { 123 };
    result.constraints.push_back ({ 777, OperationKind::ProtectedZone, { { 1, 2, 3 } }, {}, {} });
    return result;
}
void ExpectSentinel (const LandscapeTerrainResult& result)
{
    EXPECT_EQ (result.landscape.revision, 999U);
    ASSERT_EQ (result.landscape.propertyLine.size (), 1U);
    EXPECT_EQ (result.landscape.propertyLine[0].x, 90);
    EXPECT_EQ (result.terrain.revision, 888U);
    EXPECT_EQ (result.terrain.points, (std::vector<double> { 123 }));
    ASSERT_EQ (result.constraints.size (), 1U);
    EXPECT_EQ (result.constraints[0].operation, 777U);
}
double Height (const LandscapeTerrainResult& result, size_t x, size_t y)
{
    return result.terrain.points.at ((y * 9 + x) * 3 + 2);
}

TEST (EditLandscapeTerrain, AreaGeometryAndPropertyComeFromTheAuthoritativeDocument)
{
    LandscapeFixture fixture;
    const Id root = fixture.Select (fixture.footprint);
    fixture.Capture ();
    const auto before = fixture.doc.landscape;
    const auto points = fixture.points;
    const auto input = fixture.Input ();
    auto edit = fixture.Area (root);
    // A stale caller footprint is not authoritative and cannot redirect the area.
    const std::array<double, 9> distant { 40, 40, 0, 50, 40, 0, 45, 50, 0 };
    edit.parameters.geometry = distant;
    const std::array<EditTerrainOperation, 1> edits { edit };
    const std::array<double, 4> queries { 4, 4, 100, 100 };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    request.queriesXY = queries;
    request.volumes.emplace ();
    LandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_TRUE (error.empty ());
    EXPECT_EQ (Height (result, 4, 4), 5);
    EXPECT_DOUBLE_EQ (Height (result, 0, 4), 0.8); // Property boundary stays at baseline.
    EXPECT_DOUBLE_EQ (Height (result, 8, 4), 1.6);
    EXPECT_EQ (result.terrain.triangles, fixture.triangles);
    EXPECT_EQ (result.terrain.inputProvenance, fixture.provenance);
    EXPECT_EQ (result.terrain.vertexOperations[4 * 9 + 4], root);
    ASSERT_EQ (result.terrain.samples.size (), 2U);
    EXPECT_EQ (result.terrain.samples[0].height, 5);
    EXPECT_FALSE (result.terrain.samples[1].onSurface);
    EXPECT_TRUE (result.constraints.empty ());
    EXPECT_EQ (fixture.doc.landscape, before);
    EXPECT_EQ (fixture.points, points);
    ASSERT_TRUE (result.terrain.volumes);
    EXPECT_GT (result.terrain.volumes->site.fill, 0);
    EXPECT_EQ (result.terrain.volumes->context.fill, 0);
}

TEST (EditLandscapeTerrain, GradingLimitAndProtectionConsultBaselineAndProposedHeights)
{
    LandscapeFixture fixture;
    const Id limit = fixture.Operation (OperationKind::GradingLimit, { fixture.footprint, fixture.Square (2, 2, 4) });
    const Id root = fixture.Operation (OperationKind::ProtectedZone, { limit, fixture.Square (3, 3, 1) });
    fixture.Capture ();
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    request.smoothing = 0.5;
    request.volumes.emplace ();
    LandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_GT (Height (result, 5, 5), 1.5);
    for (size_t y = 0; y < 9; ++y)
        for (size_t x = 0; x < 9; ++x) {
            const bool excluded = x <= 2 || x >= 6 || y <= 2 || y >= 6 || (x >= 3 && x <= 4 && y >= 3 && y <= 4);
            if (excluded)
                EXPECT_DOUBLE_EQ (Height (result, x, y), 0.1 * double (x) + 0.2 * double (y));
        }
    ASSERT_EQ (result.constraints.size (), 2U);
    EXPECT_EQ (result.constraints[0].operation, limit);
    EXPECT_EQ (result.constraints[1].operation, root);
    for (const auto& constraint : result.constraints) {
        ASSERT_FALSE (constraint.points.empty ());
        ASSERT_EQ (constraint.baseline.size (), constraint.points.size ());
        ASSERT_EQ (constraint.proposed.size (), constraint.points.size ());
        for (size_t i = 0; i < constraint.points.size (); ++i) {
            EXPECT_TRUE (constraint.baseline[i].onSurface);
            EXPECT_NEAR (constraint.baseline[i].height, 0.1 * constraint.points[i].x + 0.2 * constraint.points[i].y,
                         1e-12);
            EXPECT_NEAR (constraint.proposed[i].height, constraint.baseline[i].height, 1e-12);
        }
    }
    EXPECT_TRUE (result.terrain.samples.empty ()) << "Internal mask queries are not user samples";
}

TEST (EditLandscapeTerrain, ProtectedHoleRemainsEditableAndMaterialMaskDoesNotBlockTerrain)
{
    LandscapeFixture fixture;
    const Id protectedOuter = fixture.Square (2, 2, 4), hole = fixture.Square (3, 3, 2);
    const Id annulus = fixture.Operation (OperationKind::Difference, { protectedOuter, hole });
    const Id protectedRoot = fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, annulus });
    const Id root = fixture.Operation (OperationKind::MaterialMask, { protectedRoot, hole });
    fixture.Capture ();
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    LandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (Height (result, 4, 4), 5);
    EXPECT_DOUBLE_EQ (Height (result, 2, 4), 1.0);
    EXPECT_EQ (result.constraints.size (), 1U);
    EXPECT_EQ (result.constraints[0].operation, protectedRoot);
}

TEST (EditLandscapeTerrain, DisabledAndUnreachedConstraintsAndUnrelatedBranchesDoNotLeak)
{
    LandscapeFixture fixture;
    const Id disabled =
        fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, fixture.Square (3, 3, 2) });
    std::string error;
    ASSERT_TRUE (SetLandscapeOperationEnabled (fixture.doc, disabled, false, error)) << error;
    const Id root = fixture.Operation (OperationKind::GradingLimit, { disabled, fixture.Square (20, 20, 2) });
    fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, fixture.Square (3, 3, 2) });
    fixture.Capture ();
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    LandscapeTerrainResult result;
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (Height (result, 4, 4), 5);
    EXPECT_TRUE (result.constraints.empty ());
    EXPECT_EQ (result.landscape.terrainConstraints.size (), 1U) << "Only the unrelated enabled constraint is derived";
}

TEST (EditLandscapeTerrain, MixedScopesRefuseInsteadOfApplyingOneObjectsMaskToAnother)
{
    LandscapeFixture fixture;
    const Id constrained =
        fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, fixture.Square (3, 3, 2) });
    const Id plain = fixture.Select (fixture.footprint);
    const Id mixed = fixture.Operation (OperationKind::Union, { constrained, plain });
    fixture.Capture ();
    const auto input = fixture.Input ();
    auto result = Sentinel ();
    std::string error;
    std::array<EditTerrainOperation, 2> edits { fixture.Area (constrained), fixture.Area (plain) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("TerrainConstraintScopeUnsupported:"), 0U) << error;
    ExpectSentinel (result);
    const std::array<EditTerrainOperation, 1> unionEdit { fixture.Area (mixed) };
    request.edits = unionEdit;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("TerrainConstraintScopeUnsupported:"), 0U) << error;
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, DisconnectedRegionsWithDifferentReachedMasksDoNotAcquireOneGlobalScope)
{
    LandscapeFixture fixture;
    const Id base = fixture.Operation (OperationKind::Union, { fixture.Square (1, 1, 2), fixture.Square (5, 1, 2) });
    const Id root = fixture.Operation (OperationKind::GradingLimit, { base, fixture.Square (1, 1, 1) });
    fixture.Capture ();
    const auto input = fixture.Input ();
    TerrainEdit point;
    point.position = { 6, 2, 5 };
    point.blur = 1;
    point.maxDistance = 10;
    const std::array<EditTerrainOperation, 1> edits { { { root, point } } };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("TerrainConstraintScopeUnsupported:"), 0U) << error;
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, MultipleEditsWithOneCommonScopeKeepTheirOperationIds)
{
    LandscapeFixture fixture;
    const Id constrained =
        fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, fixture.Square (3, 3, 2) });
    const Id root = fixture.Select (constrained);
    fixture.Capture ();
    const auto input = fixture.Input ();
    TerrainEdit fill;
    fill.position = { 2, 4, 5 };
    fill.blur = 1;
    fill.maxDistance = 10;
    TerrainEdit cut = fill;
    cut.position = { 6, 4, -5 };
    const std::array<EditTerrainOperation, 2> edits { { { constrained, fill }, { root, cut } } };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    request.volumes.emplace ();
    LandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (Height (result, 2, 4), 5);
    EXPECT_EQ (Height (result, 6, 4), -5);
    EXPECT_DOUBLE_EQ (Height (result, 4, 4), 1.2);
    EXPECT_EQ (result.terrain.vertexOperations[4 * 9 + 2], constrained);
    EXPECT_EQ (result.terrain.vertexOperations[4 * 9 + 6], root);
    ASSERT_TRUE (result.terrain.volumes);
    ASSERT_EQ (result.terrain.volumes->operations.size (), 2U);
    EXPECT_EQ (result.terrain.volumes->operations[0].operation, constrained);
    EXPECT_EQ (result.terrain.volumes->operations[1].operation, root);
}

TEST (EditLandscapeTerrain, HoleBearingAreaRefusesRatherThanFillingItsHole)
{
    LandscapeFixture fixture;
    const Id root = fixture.Operation (OperationKind::Difference, { fixture.footprint, fixture.Square (3, 3, 2) });
    fixture.Capture ();
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("TerrainFootprintUnsupported:"), 0U) << error;
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, MissingPropertyStaleCaptureAndLateRefusalsPublishNothing)
{
    LandscapeFixture fixture;
    const Id root = fixture.Select (fixture.footprint);
    fixture.Capture ();
    auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    auto result = Sentinel ();
    std::string error;
    ++input.revision;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("ProvenanceStale:"), 0U);
    --input.revision;
    const auto property = fixture.doc.landscape->propertyLineSourceId;
    fixture.doc.landscape->propertyLineSourceId = 0;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("PropertyLineOutside:"), 0U) << error;
    fixture.doc.landscape->propertyLineSourceId = property;
    const std::array<double, 3> odd { 1, 1, 1 };
    request.queriesXY = odd;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    request.queriesXY = {};
    request.volumes = EditTerrainVolumeOptions { 0, 1 };
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, BaselineCoverageMissOnAnEffectiveConstraintIsNotAFakeHeight)
{
    LandscapeFixture fixture;
    // Move the property beyond the terrain while retaining a valid planar document.
    auto* entity = edit::Find (fixture.doc, fixture.doc.landscape->sources.front ().entityId);
    entity->vertices[1].p.x = 12;
    entity->vertices[2].p.x = 12;
    const Id root = fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, fixture.Square (6, 2, 4) });
    fixture.Capture ();
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("TerrainCoverageMissing:"), 0U) << error;
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, MissingGuideCatalogAndStaleOperationParentsRefuse)
{
    LandscapeFixture fixture;
    const Id mask = fixture.Square (3, 3, 2);
    const Id constrained = fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, mask });
    const Id root = fixture.Select (constrained);
    fixture.Capture ();
    const auto original = fixture.provenance;
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    auto result = Sentinel ();
    std::string error;
    std::erase_if (fixture.provenance.table.sources, [=] (const auto& row) { return row.id == mask; });
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("ProvenanceUnattributable:"), 0U) << error;
    ExpectSentinel (result);
    fixture.provenance = original;
    fixture.provenance.table.operations.back ().parents = {};
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("ProvenanceStale:"), 0U) << error;
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, NativeGuideBindingMustBeRetainedRatherThanInvented)
{
    LandscapeFixture fixture;
    const Id root = fixture.Select (fixture.footprint);
    const std::string guid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
    fixture.doc.landscape->sources.front ().archicadGuid = guid;
    fixture.Capture ();
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area (root) };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("ProvenanceUnattributable:"), 0U) << error;
    ExpectSentinel (result);
    auto& table = fixture.provenance.table;
    NativeBinding binding;
    binding.logicalKind = LogicalKind::Source;
    binding.logicalId = fixture.property;
    std::copy (guid.begin (), guid.end (), binding.nativeGuid.begin ());
    binding.role = { 0, 5 };
    table.roleBytes = { 'g', 'u', 'i', 'd', 'e' };
    table.bindings = { binding };
    table.bindingRefs = { 0 };
    table.sources.front ().bindings = { 0, 1 };
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (result.terrain.inputProvenance.table.bindings, table.bindings);
}

TEST (EditLandscapeTerrain, SmoothingWithoutAnAttributedScopeRefuses)
{
    LandscapeFixture fixture;
    fixture.Select (fixture.footprint);
    fixture.Capture ();
    const auto input = fixture.Input ();
    LandscapeTerrainRequest request { input };
    request.smoothing = 1;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("ProvenanceUnattributable:"), 0U) << error;
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, RepeatedDerivedAreasRefuseAtTheCompilationBudgetWithoutPartialOutput)
{
    LandscapeFixture fixture;
    const Id hexagon = fixture.Ring ({ { 2, 1, 0 }, { 6, 1, 0 }, { 7, 4, 0 }, { 6, 7, 0 }, { 2, 7, 0 }, { 1, 4, 0 } });
    std::vector<EditTerrainOperation> edits;
    // Still below the document's 4096-operation limit, but 3334 x 6 derived
    // vertices exceeds the terrain compiler's 20000-segment allocation budget.
    for (size_t i = 0; i < 3334; ++i) {
        LandscapeOperation operation;
        operation.id = edit::NewId (fixture.doc);
        operation.kind = OperationKind::RegionSelect;
        operation.inputs = { hexagon };
        fixture.doc.landscape->operations.push_back (operation);
        edits.push_back (fixture.Area (operation.id));
    }
    fixture.Capture ();
    const auto input = fixture.Input ();
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (EvaluateLandscapeTerrain (fixture.doc, request, result, error));
    EXPECT_EQ (error.find ("TerrainFootprintWorkLimit:"), 0U) << error;
    ExpectSentinel (result);
}

TEST (EditLandscapeTerrain, ReadOnlyEvaluationPreservesThePlanarDocumentAndAllowsBorrowedPriorOutput)
{
    LandscapeFixture fixture;
    fixture.Select (fixture.footprint);
    fixture.Capture ();
    const auto input = fixture.Input ();
    LandscapeResult planar;
    ASSERT_TRUE (EvaluateLandscape (fixture.doc, planar));
    LandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, { input }, result, error)) << error;
    EXPECT_EQ (result.landscape.revision, planar.revision);
    EXPECT_EQ (result.landscape.faces.size (), planar.faces.size ());
    EXPECT_EQ (result.landscape.regions.size (), planar.regions.size ());
    EXPECT_EQ (result.terrain.points, fixture.points);
    const EditTerrainInput borrowed { { result.terrain.points, result.terrain.triangles },
                                      result.terrain.inputProvenance,
                                      result.terrain.revision,
                                      result.terrain.sourceTerrainGuid };
    const std::array<double, 2> queries { 4, 4 };
    LandscapeTerrainRequest request { borrowed };
    request.queriesXY = queries;
    ASSERT_TRUE (EvaluateLandscapeTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (result.terrain.points, fixture.points);
    ASSERT_EQ (result.terrain.samples.size (), 1U);
    EXPECT_NEAR (result.terrain.samples[0].height, 1.2, 1e-12);
}
} // namespace
