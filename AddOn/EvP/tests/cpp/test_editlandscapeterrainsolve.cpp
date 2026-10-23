#include "Edit/EditLandscapeTerrain.hpp"
#include "Edit/EditDocument.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace {
using namespace tapioca::geometry;
namespace edit = geomsrv::edit;

double Surface (double x, double y)
{
    return .1 * x + .2 * y;
}

// 9x9 vertices at 1-m spacing; the property line sits on half-metre offsets so the
// Rust split creates real fragments, interpolated vertices and outside faces.
struct SolveFixture {
    edit::Document doc;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    ProvenanceRequest provenance;
    Id footprint = 0;
    ObjectId owner = 0;
    EditTerrainSplitResult split;

    SolveFixture ()
    {
        std::string error;
        EXPECT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
        const Id property = Ring ({ { .5, .5, 50 }, { 7.5, .5, 50 }, { 7.5, 7.5, 50 }, { .5, 7.5, 50 } });
        EXPECT_TRUE (SetLandscapePropertyLine (doc, property, error)) << error;
        footprint = Ring ({ { 1, 1, 0 }, { 7, 1, 0 }, { 7, 7, 0 }, { 1, 7, 0 } });
        for (uint32_t y = 0; y < 9; ++y)
            for (uint32_t x = 0; x < 9; ++x)
                points.insert (points.end (), { double (x), double (y), Surface (x, y) });
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
        const size_t mappingOperation = table.operationIds.size ();
        table.operationIds.push_back (doc.landscape->operations.front ().id);
        owner = doc.nextId + 100; // Captured terrain owner, not a document allocation.
        table.objects = { { owner, doc.landscape->operations.front ().id } };
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
                row.object = owner;
                row.predecessors = { table.predecessors.size (), 1 };
                table.predecessors.push_back ({ 90, domain, i });
                table.mappings.push_back (row);
            }
    }
    void Prepare (OperationId operation)
    {
        Capture ();
        const EditTerrainInput input {
            { points, triangles }, provenance, doc.revision, doc.landscape->sourceTerrainGuid
        };
        std::string error;
        ASSERT_TRUE (SplitEditTerrain (doc, input, { operation, owner, 101, 102 }, split, error)) << error;
    }
    uint32_t Vertex (double x, double y) const
    {
        const auto& p = split.split.points;
        for (size_t i = 0; i < p.size () / 3; ++i)
            if (p[i * 3] == x && p[i * 3 + 1] == y)
                return static_cast<uint32_t> (i);
        ADD_FAILURE () << "no prepared vertex at " << x << ", " << y;
        return 0;
    }
};

// Each unknown is pulled towards its baseline height plus `lift`; one row per unknown
// makes the certified least-squares point exactly that target.
struct Lift {
    std::vector<GradingTerm> terms;
    std::vector<GradingRow> rows;
    GradingRequest system;

    Lift (const EditTerrainSplitResult& split, const std::vector<uint32_t>& vertices, double lift)
    {
        for (uint32_t i = 0; i < vertices.size (); ++i)
            terms.push_back ({ i, 1 });
        const auto& points = split.split.points;
        for (size_t i = 0; i < vertices.size (); ++i) {
            // Out-of-range locators are the adapter's refusal to name, not the fixture's.
            const size_t at = size_t (vertices[i]) * 3 + 2;
            rows.push_back ({ std::span (terms).subspan (i, 1), (at < points.size () ? points[at] : 0) + lift, 1 });
        }
        system.unknowns = static_cast<uint32_t> (vertices.size ());
        system.rows = rows;
    }
    Lift (const Lift&) = delete;
};

SplitLandscapeSolveResult Sentinel ()
{
    SplitLandscapeSolveResult result;
    result.terrain.revision = 888;
    result.terrain.points = { 123 };
    result.preservation.outsideFaces = 77;
    return result;
}
void ExpectSentinel (const SplitLandscapeSolveResult& result)
{
    EXPECT_EQ (result.terrain.revision, 888U);
    EXPECT_EQ (result.terrain.points, (std::vector<double> { 123 }));
    EXPECT_EQ (result.preservation.outsideFaces, 77U);
    EXPECT_TRUE (result.solve.vertices.empty ());
}
std::string Refusal (const SolveFixture& fixture, OperationId operation, const std::vector<uint32_t>& vertices)
{
    const Lift lift (fixture.split, vertices, 1);
    const SplitLandscapeSolveRequest request { fixture.split, operation, vertices, lift.system };
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (ApplySplitLandscapeSolve (fixture.doc, request, result, error));
    ExpectSentinel (result);
    return error;
}

TEST (EditLandscapeTerrainSolve, CertifiedHeightsApplyInsideWhileTheExteriorStaysBitIdentical)
{
    SolveFixture fixture;
    const Id root = fixture.Operation (OperationKind::RegionSelect, { fixture.footprint });
    fixture.Prepare (root);
    std::vector<uint32_t> vertices;
    for (int y = 2; y <= 6; ++y)
        for (int x = 2; x <= 6; ++x)
            vertices.push_back (fixture.Vertex (x, y));
    std::reverse (vertices.begin (), vertices.end ()); // Unknown order is the caller's.
    const Lift lift (fixture.split, vertices, 1);
    const std::array<double, 4> queries { 4, 4, .25, .25 };
    SplitLandscapeSolveRequest request { fixture.split, root, vertices, lift.system };
    request.queriesXY = queries;
    request.volumes.emplace ();
    SplitLandscapeSolveResult result;
    std::string error;
    ASSERT_TRUE (ApplySplitLandscapeSolve (fixture.doc, request, result, error)) << error;

    EXPECT_GT (result.preservation.outsideFaces, 0U);
    EXPECT_EQ (result.preservation.propertyEdges, fixture.split.propertyEdges.size ());
    EXPECT_EQ (result.solve.vertices, vertices);
    EXPECT_EQ (result.solve.operation, root);
    const auto& terrain = result.terrain;
    EXPECT_EQ (terrain.triangles, fixture.split.split.triangles);
    EXPECT_EQ (terrain.inputProvenance, fixture.split.meshProvenance);
    ASSERT_EQ (terrain.vertexOperations.size (), fixture.split.split.points.size () / 3);
    size_t moved = 0;
    for (size_t i = 0; i < terrain.vertexOperations.size (); ++i) {
        const bool unknown = std::find (vertices.begin (), vertices.end (), uint32_t (i)) != vertices.end ();
        const double expected = fixture.split.split.points[i * 3 + 2] + (unknown ? 1 : 0);
        EXPECT_NEAR (terrain.points[i * 3 + 2], expected, 1e-9);
        EXPECT_EQ (terrain.vertexOperations[i].has_value (), unknown);
        if (terrain.vertexOperations[i]) {
            EXPECT_EQ (*terrain.vertexOperations[i], root);
            ++moved;
        }
    }
    EXPECT_EQ (moved, vertices.size ());
    ASSERT_EQ (terrain.reports.size (), 1U);
    EXPECT_EQ (terrain.reports[0].operation, root);
    EXPECT_EQ (terrain.reports[0].report.moved, vertices.size ());
    std::vector<EditTerrainFaceOwner> faces;
    const auto& tri = fixture.split.split.triangles;
    for (uint32_t face = 0; face < tri.size () / 3; ++face)
        if (std::any_of (tri.begin () + face * 3, tri.begin () + face * 3 + 3, [&] (uint32_t v) {
                return std::find (vertices.begin (), vertices.end (), v) != vertices.end ();
            }))
            faces.push_back ({ face, root });
    EXPECT_EQ (terrain.faces, faces);
    EXPECT_EQ (faces.size (), 70U); // The 6x6 cells around the lifted block, less two untouched corners.
    EXPECT_TRUE (std::all_of (terrain.faces.begin (), terrain.faces.end (),
                              [=] (const auto& face) { return face.operation == root; }));
    ASSERT_EQ (terrain.samples.size (), 2U);
    EXPECT_NEAR (terrain.samples[0].height, Surface (4, 4) + 1, 1e-9);
    EXPECT_NEAR (terrain.samples[1].height, Surface (.25, .25), 1e-12);

    // Each lifted interior vertex adds a unit hat: 1/3 of its 3 m^2 star.
    ASSERT_TRUE (terrain.volumes);
    EXPECT_NEAR (terrain.volumes->site.fill, 25, .25);
    EXPECT_NEAR (terrain.volumes->site.cut, 0, 1e-12);
    EXPECT_NEAR (terrain.volumes->context.fill, 0, 1e-12);
    EXPECT_NEAR (terrain.volumes->context.cut, 0, 1e-12);
    ASSERT_EQ (terrain.volumes->operations.size (), 1U);
    EXPECT_EQ (terrain.volumes->operations[0].operation, root);
    EXPECT_NEAR (terrain.volumes->operations[0].fill, terrain.volumes->site.fill, 1e-9);
}

TEST (EditLandscapeTerrainSolve, AuthoredFixedControlCannotDisappearInSplitSolveApplication)
{
    SolveFixture fixture;
    const Id root = fixture.Operation (OperationKind::RegionSelect, { fixture.footprint });
    const auto source = std::find_if (fixture.doc.landscape->sources.begin (), fixture.doc.landscape->sources.end (),
                                      [&] (const auto& value) { return value.id == fixture.footprint; });
    ASSERT_NE (source, fixture.doc.landscape->sources.end ());
    const auto vertex = edit::Find (fixture.doc, source->entityId)->vertices.front ().id;
    std::string error;
    ASSERT_TRUE (SetLandscapeTerrainControl (fixture.doc, fixture.footprint, vertex, true, error)) << error;
    fixture.Prepare (root);
    const std::vector<uint32_t> vertices { fixture.Vertex (3, 3) };
    EXPECT_EQ (Refusal (fixture, root, vertices).find ("TerrainControlTierUnsupported:"), 0U);
    ASSERT_TRUE (SetLandscapeTerrainControl (fixture.doc, fixture.footprint, vertex, false, error)) << error;
    fixture.Prepare (root);
    const Lift lift (fixture.split, vertices, 1);
    SplitLandscapeSolveResult result;
    ASSERT_TRUE (ApplySplitLandscapeSolve (fixture.doc, { fixture.split, root, vertices, lift.system }, result, error))
        << error;
    EXPECT_NEAR (result.terrain.points.at (vertices[0] * 3 + 2), Surface (3, 3) + 1, 1e-12);
}

TEST (EditLandscapeTerrainSolve, ProtectedZoneUnknownsRefuseOnTheirEdgeAndInside)
{
    SolveFixture fixture;
    const Id root =
        fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, fixture.Square (2.5, 2.5, 1) });
    fixture.Operation (OperationKind::ProtectedZone, { fixture.footprint, fixture.Square (5, 5, 1) });
    fixture.Prepare (root);
    EXPECT_EQ (
        Refusal (fixture, root, { fixture.Vertex (6, 6), fixture.Vertex (3, 3) }).rfind ("SolveProtectedVertex", 0),
        0U);
    const std::vector<uint32_t> free { fixture.Vertex (5, 5) }; // Unrelated branch's mask does not leak.
    const Lift lift (fixture.split, free, -.5);
    const SplitLandscapeSolveRequest request { fixture.split, root, free, lift.system };
    SplitLandscapeSolveResult result;
    std::string error;
    ASSERT_TRUE (ApplySplitLandscapeSolve (fixture.doc, request, result, error)) << error;
    EXPECT_NEAR (result.terrain.points[free[0] * 3 + 2], Surface (5, 5) - .5, 1e-9);

    SolveFixture edge;
    const Id edgeRoot = edge.Operation (OperationKind::ProtectedZone, { edge.footprint, edge.Square (3, 3, 1) });
    edge.Prepare (edgeRoot);
    EXPECT_EQ (Refusal (edge, edgeRoot, { edge.Vertex (4, 3) }).rfind ("SolveProtectedVertex", 0), 0U);
}

TEST (EditLandscapeTerrainSolve, GradingEnvelopeAndProtectionKeepDistinctDiagnostics)
{
    SolveFixture fixture;
    const Id limit = fixture.Operation (OperationKind::GradingLimit, { fixture.footprint, fixture.Square (2, 2, 4) });
    const Id root = fixture.Operation (OperationKind::ProtectedZone, { limit, fixture.Square (3.5, 3.5, 1) });
    fixture.Prepare (root);
    EXPECT_EQ (Refusal (fixture, root, { fixture.Vertex (1, 1) }).rfind ("SolveOutsideGradingLimit", 0), 0U);
    EXPECT_EQ (Refusal (fixture, root, { fixture.Vertex (2, 4) }).rfind ("SolveOutsideGradingLimit", 0), 0U);
    EXPECT_EQ (Refusal (fixture, root, { fixture.Vertex (4, 4) }).rfind ("SolveProtectedVertex", 0), 0U);
    const std::vector<uint32_t> inside { fixture.Vertex (3, 3), fixture.Vertex (5, 3) };
    const Lift lift (fixture.split, inside, .25);
    const SplitLandscapeSolveRequest request { fixture.split, root, inside, lift.system };
    SplitLandscapeSolveResult result;
    std::string error;
    ASSERT_TRUE (ApplySplitLandscapeSolve (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (result.terrain.reports.at (0).report.moved, 2U);
}

TEST (EditLandscapeTerrainSolve, OutsideAndPropertyEdgeUnknownsAreLeftToTheExteriorVerifier)
{
    SolveFixture fixture;
    const Id root = fixture.Operation (OperationKind::RegionSelect, { fixture.footprint });
    fixture.Prepare (root);
    EXPECT_EQ (
        Refusal (fixture, root, { fixture.Vertex (4, 4), fixture.Vertex (0, 0) }).rfind ("PropertyLineOutside", 0), 0U);
    EXPECT_EQ (Refusal (fixture, root, { fixture.Vertex (.5, .5) }).rfind ("PropertyLineOutside", 0), 0U);
    EXPECT_EQ (Refusal (fixture, root, { fixture.Vertex (4, .5) }).rfind ("PropertyLineOutside", 0), 0U);
}

TEST (EditLandscapeTerrainSolve, StaleDisabledAndMalformedRequestsPublishNothing)
{
    SolveFixture fixture;
    const Id root = fixture.Operation (OperationKind::RegionSelect, { fixture.footprint });
    fixture.Prepare (root);
    const uint32_t vertex = fixture.Vertex (4, 4);
    EXPECT_EQ (Refusal (fixture, root, { vertex, vertex }).rfind ("SolveShape", 0), 0U);
    EXPECT_EQ (Refusal (fixture, root, { 100000 }).rfind ("SolveShape", 0), 0U);
    EXPECT_EQ (Refusal (fixture, root + 1000, { vertex }).rfind ("ProvenanceUnattributable", 0), 0U);
    {
        const Lift lift (fixture.split, { vertex }, 1);
        const std::vector<uint32_t> two { vertex, fixture.Vertex (3, 3) };
        const SplitLandscapeSolveRequest request { fixture.split, root, two, lift.system };
        auto result = Sentinel ();
        std::string error;
        EXPECT_FALSE (ApplySplitLandscapeSolve (fixture.doc, request, result, error));
        EXPECT_EQ (error.rfind ("SolveShape", 0), 0U) << error;
        ExpectSentinel (result);
    }
    fixture.doc.landscape->operations.front ().enabled = false;
    EXPECT_EQ (Refusal (fixture, root, { vertex }).rfind ("ProvenanceUnattributable", 0), 0U);
    fixture.doc.landscape->operations.front ().enabled = true;
    ++fixture.doc.revision;
    EXPECT_EQ (Refusal (fixture, root, { vertex }).rfind ("ProvenanceStale", 0), 0U);
}

} // namespace
