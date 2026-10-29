#include "Edit/EditTerrainTriangulation.hpp"
#include "Edit/EditLandscapeTerrain.hpp"
#include "Edit/EditDocument.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cmath>

namespace {
using namespace tapioca::geometry;
namespace edit = geomsrv::edit;
constexpr SourceId kBaseline = 0x20000000000001ULL;
constexpr ObjectId kOwner = kBaseline + 1;
const std::string kGuideGuid = "AABBCCDD-1122-3344-5566-778899AABBCC";

double Surface (double x, double y)
{
    return .1 * x + .2 * y + .25 * std::min (x, y);
}
struct SplitFixture {
    edit::Document doc;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    ProvenanceRequest provenance;
    Id property = 0, propertyEntity = 0, footprint = 0, base = 0, root = 0;

    SplitFixture ()
    {
        std::string error;
        EXPECT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
        property = Source ({ { 1, 1, 80 }, { 7, 1, 80 }, { 7, 7, 80 }, { 1, 7, 80 } });
        propertyEntity = doc.landscape->sources.front ().entityId;
        doc.landscape->sources.front ().archicadGuid = kGuideGuid;
        EXPECT_TRUE (SetLandscapePropertyLine (doc, property, error)) << error;
        footprint = Source ({ { 2, 2, 0 }, { 6, 2, 0 }, { 6, 6, 0 }, { 2, 6, 0 } });
        base = Operation (footprint);
        root = Operation (base);
        doc.nextId = kOwner + 1;
        doc.landscape->sourceTerrainGuid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
        for (uint32_t y = 0; y < 5; ++y)
            for (uint32_t x = 0; x < 5; ++x)
                points.insert (points.end (), { double (x * 2), double (y * 2), Surface (x * 2, y * 2) });
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t a = y * 5 + x;
                triangles.insert (triangles.end (), { a, a + 1, a + 6, a, a + 6, a + 5 });
            }
        Capture ();
    }
    Id Source (std::vector<Vec3> ring)
    {
        LandscapeSource source;
        source.entityId = edit::AddPolyline (doc, ring, true);
        Id id = 0;
        std::string error;
        EXPECT_TRUE (AddLandscapeSource (doc, source, id, error)) << error;
        return id;
    }
    Id Operation (Id input)
    {
        LandscapeOperation operation;
        operation.kind = OperationKind::RegionSelect;
        operation.inputs = { input };
        Id id = 0;
        std::string error;
        EXPECT_TRUE (AddLandscapeOperation (doc, operation, id, error)) << error;
        return id;
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
        table.sources.push_back ({ kBaseline, {} });
        table.operations = { { base, {} }, { root, { 0, 1 } } };
        table.operationIds = { base };
        table.objects = { { kOwner, base } };
        table.sourceIds = { kBaseline };
        table.supportTables = { { { 1, provenance.expectedSession, doc.revision, 90 }, provenance.counts, true } };
        NativeBinding owner;
        owner.logicalKind = LogicalKind::Object;
        owner.logicalId = kOwner;
        std::copy (doc.landscape->sourceTerrainGuid.begin (), doc.landscape->sourceTerrainGuid.end (),
                   owner.nativeGuid.begin ());
        owner.metadataPresent = true;
        owner.metadataUuid = { 9 };
        owner.role = { 0, 7 };
        NativeBinding guide;
        guide.logicalKind = LogicalKind::Source;
        guide.logicalId = property;
        std::copy (kGuideGuid.begin (), kGuideGuid.end (), guide.nativeGuid.begin ());
        guide.role = { 7, 5 };
        table.roleBytes = { 't', 'e', 'r', 'r', 'a', 'i', 'n', 'g', 'u', 'i', 'd', 'e' };
        table.bindings = { owner, guide };
        table.bindingRefs = { 1 };
        table.sources.front ().bindings = { 0, 1 };
        for (auto [domain, count] : { std::pair { IdentityDomain::Vertex, provenance.counts.vertices },
                                      std::pair { IdentityDomain::Face, provenance.counts.faces } })
            for (uint64_t i = 0; i < count; ++i) {
                EntityMapping row;
                row.domain = domain;
                row.kind = MappingKind::Exact;
                row.outputIndex = i;
                row.sources = { 0, 1 };
                row.operations = { 0, 1 };
                row.object = kOwner;
                row.predecessors = { table.predecessors.size (), 1 };
                table.predecessors.push_back ({ 90, domain, i });
                table.mappings.push_back (row);
            }
    }
    EditTerrainInput Input () const
    {
        return { { points, triangles }, provenance, doc.revision, doc.landscape->sourceTerrainGuid };
    }
    EditTerrainSplitOptions Options () const
    {
        return { root, kOwner, 101, 102 };
    }
    void Property (const std::array<Vec3, 4>& ring)
    {
        auto* entity = edit::Find (doc, propertyEntity);
        for (size_t i = 0; i < ring.size (); ++i)
            entity->vertices[i].p = ring[i];
        ++doc.revision;
        Capture ();
    }
    EditTerrainOperation Area () const
    {
        TerrainEdit edit;
        edit.target = TerrainEditTarget::Area;
        edit.level = 8;
        edit.blur = 1;
        edit.maxDistance = 20;
        return { root, edit };
    }
};

const EntityMapping& Row (const ProvenanceRequest& provenance, IdentityDomain domain, uint64_t index)
{
    return *std::find_if (provenance.table.mappings.begin (), provenance.table.mappings.end (),
                          [=] (const auto& row) { return row.domain == domain && row.outputIndex == index; });
}
bool Contains (const std::vector<uint64_t>& pool, IdentityRange range, uint64_t id)
{
    return std::find (pool.begin () + range.offset, pool.begin () + range.offset + range.count, id) !=
           pool.begin () + range.offset + range.count;
}
double Area (const TerrainResult& split, size_t face)
{
    const size_t a = split.triangles.at (face * 3) * 3, b = split.triangles.at (face * 3 + 1) * 3,
                 c = split.triangles.at (face * 3 + 2) * 3;
    return .5 * ((split.points[b] - split.points[a]) * (split.points[c + 1] - split.points[a + 1]) -
                 (split.points[b + 1] - split.points[a + 1]) * (split.points[c] - split.points[a]));
}

TEST (EditTerrainTriangulation, SplitRetainsTheSurfaceAndResolvesAllSupportsAndNativeCatalogs)
{
    SplitFixture fixture;
    const uint64_t patch = kOwner + 5;
    fixture.doc.nextId = patch + 1;
    for (auto& row : fixture.provenance.table.mappings)
        if (row.domain == IdentityDomain::Face)
            row.patchKey = patch;
    const auto original = fixture.provenance;
    const auto before = fixture.doc.landscape;
    const auto input = fixture.Input ();
    EditTerrainSplitResult result;
    std::string error;
    ASSERT_TRUE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error)) << error;
    EXPECT_EQ (result.baselineProvenance, original);
    EXPECT_EQ (fixture.provenance, original);
    EXPECT_EQ (fixture.doc.landscape, before);
    EXPECT_EQ (result.split.inputProvenance.domains, 7U);
    EXPECT_EQ (result.meshProvenance.domains, 5U);
    EXPECT_EQ (result.split.inputProvenance.table.context.tableId, 101U);
    EXPECT_EQ (result.meshProvenance.table.context.tableId, 102U);
    EXPECT_EQ (result.meshProvenance.table.bindings, original.table.bindings);
    EXPECT_EQ (result.meshProvenance.table.bindingRefs, original.table.bindingRefs);
    EXPECT_EQ (result.meshProvenance.table.roleBytes, original.table.roleBytes);
    EXPECT_EQ (result.baselineTriangles, fixture.triangles);
    EXPECT_GT (result.split.points.size (), fixture.points.size ());
    EXPECT_EQ (result.parents.size (), result.split.triangles.size () / 3);
    EXPECT_EQ (result.inside.size (), result.parents.size ());
    double inside = 0, outside = 0;
    std::vector<double> parentAreas (fixture.triangles.size () / 3);
    size_t untouched = 0;
    for (size_t i = 0; i < result.parents.size (); ++i) {
        (result.inside[i] ? inside : outside) += Area (result.split, i);
        parentAreas.at (result.parents[i]) += Area (result.split, i);
        const auto& row = Row (result.meshProvenance, IdentityDomain::Face, i);
        EXPECT_EQ (row.patchKey, patch);
        if (row.kind == MappingKind::Exact) {
            ++untouched;
            EXPECT_TRUE (std::equal (result.split.triangles.begin () + i * 3,
                                     result.split.triangles.begin () + i * 3 + 3,
                                     fixture.triangles.begin () + result.parents[i] * 3));
        }
        else {
            EXPECT_EQ (row.kind, MappingKind::Split);
            EXPECT_TRUE (Contains (result.meshProvenance.table.sourceIds, row.sources, fixture.property));
            EXPECT_TRUE (Contains (result.meshProvenance.table.operationIds, row.operations, fixture.root));
        }
    }
    EXPECT_GT (untouched, 0U);
    EXPECT_NEAR (inside, 36, 1e-12);
    EXPECT_NEAR (outside, 28, 1e-12);
    for (double area : parentAreas)
        EXPECT_NEAR (area, 2, 1e-12);
    for (size_t i = 0; i < result.split.points.size () / 3; ++i) {
        const auto& row = Row (result.meshProvenance, IdentityDomain::Vertex, i);
        EXPECT_EQ (row.object, kOwner);
        EXPECT_NEAR (result.split.points[i * 3 + 2],
                     Surface (result.split.points[i * 3], result.split.points[i * 3 + 1]), 1e-12);
        if (i < fixture.points.size () / 3)
            EXPECT_EQ (row.kind, MappingKind::Exact);
        else {
            EXPECT_EQ (row.kind, MappingKind::Split);
            EXPECT_GE (row.predecessors.count, 2U);
            EXPECT_TRUE (Contains (result.meshProvenance.table.sourceIds, row.sources, fixture.property));
            EXPECT_TRUE (Contains (result.meshProvenance.table.sourceIds, row.sources, kBaseline));
            EXPECT_TRUE (Contains (result.meshProvenance.table.operationIds, row.operations, fixture.root));
        }
    }
    const auto* entity = edit::Find (fixture.doc, fixture.propertyEntity);
    ASSERT_EQ (result.propertyVertices.size (), 4U);
    ASSERT_EQ (result.propertySegments.size (), 4U);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ (result.propertyVertices[i], entity->vertices[i].id);
        EXPECT_EQ (result.propertySegments[i], entity->segments[i].id);
    }
    EXPECT_FALSE (result.propertyEdges.empty ());
    ProvenanceDiagnostic diagnostic;
    ASSERT_TRUE (ValidateProvenance (result.meshProvenance, diagnostic, error)) << error;
    const std::array<double, 4> queries { 1.5, 1.5, 6.5, 2.5 };
    const EditTerrainInput prepared { { result.split.points, result.split.triangles },
                                      result.meshProvenance,
                                      result.revision,
                                      result.sourceTerrainGuid };
    EditTerrainRequest request { prepared };
    request.queriesXY = queries;
    EditTerrainResult sampled;
    ASSERT_TRUE (EvaluateEditTerrain (fixture.doc, request, sampled, error)) << error;
    EXPECT_NEAR (sampled.samples[0].height, Surface (1.5, 1.5), 1e-12);
    EXPECT_NEAR (sampled.samples[1].height, Surface (6.5, 2.5), 1e-12);
}

TEST (EditTerrainTriangulation, DerivedRingRetainsTerrainOwnerWhenBoundaryOperationIsIndependent)
{
    SplitFixture fixture;
    const auto boundaryOperation = fixture.Operation (fixture.footprint);
    fixture.Capture ();
    fixture.provenance.table.operations.push_back ({ boundaryOperation, {} });
    // A second object's boundary is not a descendant of the terrain owner's
    // footprint. Preparation must not invent a DAG edge to make it one.
    const auto before = fixture.doc.landscape;
    const auto original = fixture.provenance;
    auto options = fixture.Options ();
    options.operation = boundaryOperation;
    const std::array<Vec3, 4> ring { Vec3 { 2, 2, 0 }, { 6, 2, 0 }, { 6, 6, 0 }, { 2, 6, 0 } };
    const std::array<SourceId, 1> sources { fixture.footprint };
    EditTerrainSplitResult result;
    std::string error;
    ASSERT_TRUE (SplitEditTerrainRing (fixture.doc, fixture.Input (), options, ring, sources, result, error)) << error;
    const auto& combined = result.split.inputProvenance;
    for (const auto& row : combined.table.mappings)
        if (row.domain == IdentityDomain::Edge) {
            EXPECT_EQ (row.object, kOwner);
            EXPECT_TRUE (Contains (combined.table.operationIds, row.operations, fixture.base));
            EXPECT_TRUE (Contains (combined.table.operationIds, row.operations, boundaryOperation));
        }
    ProvenanceDiagnostic diagnostic;
    EXPECT_TRUE (ValidateProvenance (result.meshProvenance, diagnostic, error)) << error;
    EXPECT_EQ (diagnostic, ProvenanceDiagnostic::None);
    EXPECT_EQ (fixture.provenance, original);
    EXPECT_EQ (fixture.doc.landscape, before);
    EXPECT_TRUE (std::equal (fixture.points.begin (), fixture.points.end (), result.split.points.begin ()));
}

TEST (EditTerrainTriangulation, SplitFirstEvaluationPreservesEveryOutsideFragmentAndPropertyEdge)
{
    SplitFixture fixture;
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area () };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    request.smoothing = .5;
    request.volumes.emplace ();
    const std::array<double, 2> queries { 4, 4 };
    request.queriesXY = queries;
    SplitLandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateSplitLandscapeTerrain (fixture.doc, request, fixture.Options (), result, error)) << error;
    EXPECT_GT (result.preservation.outsideFaces, 0U);
    EXPECT_GT (result.preservation.fixedVertices, 0U);
    EXPECT_EQ (result.preservation.propertyEdges, result.baseline.propertyEdges.size ());
    const auto& proposed = result.evaluation.terrain;
    EXPECT_EQ (proposed.triangles, result.baseline.split.triangles);
    EXPECT_EQ (proposed.inputProvenance, result.baseline.meshProvenance);
    EXPECT_GT (proposed.samples[0].height, Surface (4, 4));
    for (size_t i = 0; i < result.baseline.inside.size (); ++i)
        if (!result.baseline.inside[i])
            for (size_t j = 0; j < 3; ++j) {
                const size_t vertex = proposed.triangles[i * 3 + j];
                for (size_t axis = 0; axis < 3; ++axis)
                    EXPECT_EQ (std::bit_cast<uint64_t> (proposed.points[vertex * 3 + axis]),
                               std::bit_cast<uint64_t> (result.baseline.split.points[vertex * 3 + axis]));
            }
    ASSERT_TRUE (proposed.volumes);
    EXPECT_NEAR (proposed.volumes->context.cut, 0, 1e-12);
    EXPECT_NEAR (proposed.volumes->context.fill, 0, 1e-12);
    EXPECT_GT (proposed.volumes->site.fill, 0);
}

TEST (EditTerrainTriangulation, AlreadyConformingOuterBoundaryStillPinsItsVerticesWithoutOutsideFaces)
{
    SplitFixture fixture;
    fixture.Property ({ Vec3 { 0, 0, 20 }, { 8, 0, 20 }, { 8, 8, 20 }, { 0, 8, 20 } });
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area () };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    SplitLandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateSplitLandscapeTerrain (fixture.doc, request, fixture.Options (), result, error)) << error;
    EXPECT_EQ (result.preservation.outsideFaces, 0U);
    EXPECT_EQ (result.preservation.fixedVertices, 16U);
    EXPECT_EQ (result.baseline.split.points, fixture.points);
    auto moved = result.evaluation.terrain;
    moved.points[result.baseline.propertyEdges.front ().a * 3 + 2] += 1;
    const auto before = result.preservation;
    EXPECT_FALSE (VerifyEditTerrainOutside (result.baseline, moved, result.preservation, error));
    EXPECT_EQ (error.find ("PropertyLineOutside:"), 0U) << error;
    EXPECT_EQ (result.preservation, before);
}

TEST (EditTerrainTriangulation, OutsideVerifierRefusesChangedFramingTopologyOrOutsideCoordinates)
{
    SplitFixture fixture;
    const auto input = fixture.Input ();
    SplitLandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateSplitLandscapeTerrain (fixture.doc, { input }, fixture.Options (), result, error)) << error;
    EditTerrainPreservation report { 77, 88, 99 };
    const auto before = report;
    for (int kind = 0; kind < 5; ++kind) {
        auto proposed = result.evaluation.terrain;
        if (kind == 0)
            ++proposed.inputProvenance.expectedRevision;
        if (kind == 1)
            std::swap (proposed.triangles[0], proposed.triangles[1]);
        if (kind == 2)
            proposed.points[0] += 1;
        if (kind == 3)
            proposed.points[2] += 1;
        if (kind == 4)
            proposed.points.pop_back ();
        EXPECT_FALSE (VerifyEditTerrainOutside (result.baseline, proposed, report, error)) << kind;
        EXPECT_EQ (report, before);
    }
}

TEST (EditTerrainTriangulation, MovedAndObliqueLinesResplitTheOriginalBaselineDeterministically)
{
    SplitFixture fixture;
    fixture.Property ({ Vec3 { 4, .75, 90 }, { 7.25, 4, 90 }, { 4, 7.25, 90 }, { .75, 4, 90 } });
    const auto input = fixture.Input ();
    EditTerrainSplitResult first, second;
    std::string error;
    ASSERT_TRUE (SplitEditTerrain (fixture.doc, input, fixture.Options (), first, error)) << error;
    ASSERT_TRUE (SplitEditTerrain (fixture.doc, input, fixture.Options (), second, error)) << error;
    EXPECT_EQ (first, second);
    double inside = 0;
    for (size_t i = 0; i < first.inside.size (); ++i)
        if (first.inside[i])
            inside += Area (first.split, i);
    EXPECT_NEAR (inside, 2 * 3.25 * 3.25, 1e-12);
    for (size_t i = 0; i < first.split.points.size (); i += 3)
        EXPECT_NEAR (first.split.points[i + 2], Surface (first.split.points[i], first.split.points[i + 1]), 1e-12);
}

TEST (EditTerrainTriangulation, RevisionZeroAndSurveyFramingDoNotNarrowCoordinatesOrIds)
{
    SplitFixture fixture;
    for (size_t i = 0; i < fixture.points.size (); i += 3) {
        fixture.points[i] += 500000;
        fixture.points[i + 1] += 6000000;
    }
    for (auto& entity : fixture.doc.entities)
        for (auto& vertex : entity.vertices) {
            vertex.p.x += 500000;
            vertex.p.y += 6000000;
        }
    fixture.doc.revision = 0;
    fixture.Capture ();
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area () };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    SplitLandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateSplitLandscapeTerrain (fixture.doc, request, fixture.Options (), result, error)) << error;
    EXPECT_EQ (result.baseline.revision, 0U);
    EXPECT_EQ (result.baseline.meshProvenance.table.objects.front ().id, kOwner);
    EXPECT_EQ (result.baseline.split.points.front (), 500000);
    EXPECT_EQ (result.baseline.split.points[1], 6000000);
    EXPECT_GT (result.preservation.outsideFaces, 0U);
}

TEST (EditTerrainTriangulation, ClockwisePropertyEdgesRemainConnectedAndExteriorIsPreserved)
{
    SplitFixture fixture;
    fixture.Property ({ Vec3 { 1, 1, 90 }, { 1, 7, 90 }, { 7, 7, 90 }, { 7, 1, 90 } });
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area () };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    SplitLandscapeTerrainResult result;
    std::string error;
    ASSERT_TRUE (EvaluateSplitLandscapeTerrain (fixture.doc, request, fixture.Options (), result, error)) << error;
    double inside = 0;
    for (size_t i = 0; i < result.baseline.inside.size (); ++i)
        if (result.baseline.inside[i])
            inside += Area (result.baseline.split, i);
    EXPECT_NEAR (inside, 36, 1e-12);
    EXPECT_GT (result.preservation.outsideFaces, 0U);
}

TEST (EditTerrainTriangulation, InteriorGuideVerticesKeepTheirFaceAndGuideSupports)
{
    SplitFixture fixture;
    fixture.Property ({ Vec3 { 1, .5, 80 }, { 7, .5, 80 }, { 7, 7.5, 80 }, { 1, 7.5, 80 } });
    const auto input = fixture.Input ();
    EditTerrainSplitResult result;
    std::string error;
    ASSERT_TRUE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error)) << error;
    size_t faceHosted = 0;
    for (const auto& support : result.split.mappings)
        if (support.kind == TerrainMappingKind::SplitVertex && support.values[2] == 2) {
            ++faceHosted;
            const auto& row = Row (result.meshProvenance, IdentityDomain::Vertex, support.index);
            ASSERT_EQ (row.predecessors.count, 2U);
            const auto& face = result.meshProvenance.table.predecessors.at (row.predecessors.offset);
            EXPECT_EQ (face.domain, IdentityDomain::Face);
            EXPECT_EQ (face.entityIndex, support.values[3]);
            const auto& guide = result.meshProvenance.table.predecessors.at (row.predecessors.offset + 1);
            EXPECT_EQ (guide.domain, IdentityDomain::Vertex);
            EXPECT_EQ (guide.entityIndex, fixture.provenance.counts.vertices + support.values[1]);
            EXPECT_NEAR (result.split.points[support.index * 3 + 2],
                         Surface (result.split.points[support.index * 3], result.split.points[support.index * 3 + 1]),
                         1e-12);
        }
    EXPECT_EQ (faceHosted, 4U);
}

TEST (EditTerrainTriangulation, VerifierRejectsUnpreparedAndMalformedSplitMetadata)
{
    EditTerrainPreservation report { 77, 88, 99 };
    const auto before = report;
    std::string error;
    EXPECT_FALSE (VerifyEditTerrainOutside ({}, {}, report, error));
    EXPECT_EQ (report, before);
    SplitFixture fixture;
    const auto input = fixture.Input ();
    SplitLandscapeTerrainResult result;
    ASSERT_TRUE (EvaluateSplitLandscapeTerrain (fixture.doc, { input }, fixture.Options (), result, error)) << error;
    result.baseline.inside.front () = 2;
    EXPECT_FALSE (VerifyEditTerrainOutside (result.baseline, result.evaluation.terrain, report, error));
    EXPECT_EQ (error.find ("TerrainOutputShape:"), 0U) << error;
    EXPECT_EQ (report, before);
}

TEST (EditTerrainTriangulation, APropertySegmentAcrossAMeshHoleIsNotDeclaredCovered)
{
    SplitFixture fixture;
    // Remove the cell x=2..4, y=2..4; the property vertices are covered but
    // its lower segment y=3 crosses this missing piece of terrain.
    fixture.triangles.erase (fixture.triangles.begin () + 5 * 6, fixture.triangles.begin () + 6 * 6);
    fixture.Property ({ Vec3 { 1, 3, 0 }, { 7, 3, 0 }, { 7, 5, 0 }, { 1, 5, 0 } });
    const auto input = fixture.Input ();
    EditTerrainSplitResult result;
    result.revision = 777;
    const auto before = result;
    std::string error;
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_EQ (error.find ("PropertyLineOutside"), 0U) << error;
    EXPECT_EQ (result, before);
}

TEST (EditTerrainTriangulation, InvalidTokensOwnerAndStaleInputsRefuseWithoutPartialOutput)
{
    SplitFixture fixture;
    auto input = fixture.Input ();
    EditTerrainSplitResult result;
    result.revision = 777;
    result.split.points = { 123 };
    const auto before = result;
    std::string error;
    for (uint64_t token : { 0ULL, 90ULL, 100ULL, 102ULL }) {
        auto options = fixture.Options ();
        options.splitTable = token;
        EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, options, result, error));
        EXPECT_EQ (error.find ("ProvenanceConflict:"), 0U) << error;
        EXPECT_EQ (result, before);
    }
    auto options = fixture.Options ();
    options.object = kOwner + 1;
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, options, result, error));
    options = fixture.Options ();
    options.operation = 0;
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, options, result, error));
    ++input.revision;
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    --input.revision;
    fixture.provenance.table.operations.back ().parents = {};
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_EQ (error.find ("ProvenanceStale:"), 0U) << error;
    EXPECT_EQ (result, before);
}

TEST (EditTerrainTriangulation, MissingGuideNativeBindingOrMixedOwnersCannotBeInvented)
{
    SplitFixture fixture;
    const auto original = fixture.provenance;
    const auto input = fixture.Input ();
    EditTerrainSplitResult result;
    std::string error;
    fixture.provenance.table.sources.front ().bindings = {};
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_EQ (error.find ("ProvenanceUnattributable:"), 0U) << error;
    fixture.provenance = original;
    std::erase_if (fixture.provenance.table.sources, [&] (const auto& row) { return row.id == fixture.property; });
    // Drop the binding too so this is a structurally valid catalog missing the guide.
    fixture.provenance.table.bindings.pop_back ();
    fixture.provenance.table.bindingRefs.clear ();
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    fixture.provenance = original;
    fixture.provenance.table.objects.push_back ({ kOwner + 1, fixture.base });
    fixture.provenance.table.mappings.back ().object = kOwner + 1;
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_EQ (error.find ("ProvenanceConflict:"), 0U) << error;
    fixture.provenance = original;
    fixture.provenance.requireObject = false;
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_TRUE (result.split.points.empty ());
}

TEST (EditTerrainTriangulation, ArcMalformedAndOffMeshPropertiesKeepNamedRefusals)
{
    SplitFixture fixture;
    const auto input = fixture.Input ();
    EditTerrainSplitResult result;
    result.split.points = { 456 };
    const auto before = result;
    std::string error;
    fixture.doc.landscape->sources.front ().arcAngles = { .1, 0, 0, 0 };
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_EQ (error.find ("PropertyLineArcUnsupported:"), 0U) << error;
    fixture.doc.landscape->sources.front ().arcAngles.clear ();
    auto* entity = edit::Find (fixture.doc, fixture.propertyEntity);
    std::swap (entity->vertices[1].p, entity->vertices[2].p);
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_EQ (error.find ("PropertyLineOutside:"), 0U) << error;
    std::swap (entity->vertices[1].p, entity->vertices[2].p);
    entity->vertices[1].p.x = 10;
    entity->vertices[2].p.x = 10;
    EXPECT_FALSE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error));
    EXPECT_EQ (error.find ("PropertyLineOutside"), 0U) << error;
    EXPECT_EQ (result, before);
}

TEST (EditTerrainTriangulation, SplitEvaluationLateFailureKeepsBothBaselineAndPreviewUntouched)
{
    SplitFixture fixture;
    const auto input = fixture.Input ();
    const std::array<EditTerrainOperation, 1> edits { fixture.Area () };
    LandscapeTerrainRequest request { input };
    request.edits = edits;
    request.volumes = EditTerrainVolumeOptions { 0, 1 };
    SplitLandscapeTerrainResult result;
    result.baseline.revision = 999;
    result.baseline.split.points = { 123 };
    result.evaluation.terrain.points = { 456 };
    result.preservation = { 77, 88, 99 };
    std::string error;
    EXPECT_FALSE (EvaluateSplitLandscapeTerrain (fixture.doc, request, fixture.Options (), result, error));
    EXPECT_EQ (result.baseline.revision, 999U);
    EXPECT_EQ (result.baseline.split.points, (std::vector<double> { 123 }));
    EXPECT_EQ (result.evaluation.terrain.points, (std::vector<double> { 456 }));
    EXPECT_EQ (result.preservation, (EditTerrainPreservation { 77, 88, 99 }));
}

TEST (EditTerrainTriangulation, PriorOwnedSplitCanSupplyTheOriginalBaselineUntilPublication)
{
    SplitFixture fixture;
    fixture.points[2] = -0.0;
    const auto input = fixture.Input ();
    EditTerrainSplitResult result;
    std::string error;
    ASSERT_TRUE (SplitEditTerrain (fixture.doc, input, fixture.Options (), result, error)) << error;
    EXPECT_EQ (std::bit_cast<uint64_t> (result.split.points[2]), std::bit_cast<uint64_t> (-0.0));
    const auto before = result;
    const EditTerrainInput borrowed {
        { std::span<const double> (result.split.points).first (result.baselineProvenance.counts.vertices * 3),
          result.baselineTriangles },
        result.baselineProvenance,
        result.revision,
        result.sourceTerrainGuid
    };
    ASSERT_TRUE (SplitEditTerrain (fixture.doc, borrowed, fixture.Options (), result, error)) << error;
    EXPECT_EQ (result, before);
}
} // namespace
