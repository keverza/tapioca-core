#include "Edit/EditSparseSolve.hpp"
#include "Edit/EditDocument.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {
using namespace tapioca::geometry;
namespace edit = geomsrv::edit;
constexpr OperationId kOperation = 0x20000000000001ULL;

struct SolveFixture {
    edit::Document doc;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    ProvenanceRequest provenance;

    SolveFixture ()
    {
        doc.revision = 7;
        doc.landscape.emplace ();
        doc.landscape->sessionUuid = { 1 };
        doc.landscape->sourceTerrainGuid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
        LandscapeOperation operation;
        operation.id = kOperation;
        operation.kind = OperationKind::RegionSelect;
        doc.landscape->operations.push_back (operation);
        for (uint32_t y = 0; y < 5; ++y)
            for (uint32_t x = 0; x < 5; ++x)
                points.insert (points.end (), { double (x), double (y), 0 });
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t a = y * 5 + x;
                triangles.insert (triangles.end (), { a, a + 1, a + 6, a, a + 6, a + 5 });
            }
        provenance.expectedSession = doc.landscape->sessionUuid;
        provenance.expectedRevision = doc.revision;
        provenance.counts = { 25, 0, 32, 0 };
        auto& table = provenance.table;
        table.context = { 1, provenance.expectedSession, doc.revision, 100 };
        table.sources = { { 10, {} } };
        table.operations = { { kOperation, {} } };
        table.objects = { { 13, kOperation } };
        table.sourceIds = { 10 };
        table.operationIds = { kOperation };
        table.supportTables = { { { 1, provenance.expectedSession, 6, 90 }, provenance.counts, true } };
        NativeBinding binding;
        binding.logicalKind = LogicalKind::Object;
        binding.logicalId = 13;
        std::copy (doc.landscape->sourceTerrainGuid.begin (), doc.landscape->sourceTerrainGuid.end (),
                   binding.nativeGuid.begin ());
        binding.role = { 0, 6 };
        table.roleBytes = { 'o', 'u', 't', 'p', 'u', 't' };
        table.bindings = { binding };
        for (auto [domain, count] :
             { std::pair { IdentityDomain::Vertex, 25U }, std::pair { IdentityDomain::Face, 32U } })
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
};

struct Rows {
    std::vector<std::vector<GradingTerm>> terms;
    std::vector<GradingRow> rows;
    void Finish ()
    {
        for (const auto& row : terms)
            rows.push_back ({ row, 0, 1 });
    }
};
Rows Chain (uint32_t count)
{
    Rows rows;
    for (uint32_t i = 0; i + 1 < count; ++i)
        rows.terms.push_back ({ { i, 1 }, { i + 1, -1 } });
    rows.Finish ();
    return rows;
}
EditSparseSolveResult Sentinel ()
{
    EditSparseSolveResult result;
    result.revision = 99;
    result.sourceTerrainGuid = "untouched";
    result.operation = 777;
    result.vertices = { 333 };
    result.solution.values = { -5 };
    result.solution.objective = 123;
    result.solution.factorizations = 42;
    result.inputProvenance.expectedRevision = 456;
    return result;
}
void ExpectSentinel (const EditSparseSolveResult& result)
{
    EXPECT_EQ (result.revision, 99U);
    EXPECT_EQ (result.sourceTerrainGuid, "untouched");
    EXPECT_EQ (result.operation, 777U);
    EXPECT_EQ (result.vertices, (std::vector<uint32_t> { 333 }));
    EXPECT_EQ (result.solution.values, (std::vector<double> { -5 }));
    EXPECT_EQ (result.solution.objective, 123);
    EXPECT_EQ (result.solution.factorizations, 42U);
    EXPECT_EQ (result.inputProvenance.expectedRevision, 456U);
}

TEST (EditSparseSolve, FixedPerimeterAndRidgeGiveTheAnalyticSurfaceInEitherUnknownOrder)
{
    SolveFixture fixture;
    const auto original = fixture.points;
    const auto landscape = fixture.doc.landscape;
    const auto input = fixture.Input ();
    for (bool reverse : { false, true }) {
        SCOPED_TRACE (reverse);
        const auto index = [=] (uint32_t vertex) { return reverse ? 24U - vertex : vertex; };
        std::vector<uint32_t> vertices (25);
        Rows rows;
        std::vector<GradingFixed> fixed;
        for (uint32_t y = 0; y < 5; ++y)
            for (uint32_t x = 0; x < 5; ++x) {
                const uint32_t vertex = y * 5 + x;
                vertices[index (vertex)] = vertex;
                if (x < 4)
                    rows.terms.push_back ({ { index (vertex), 1 }, { index (vertex + 1), -1 } });
                if (y < 4)
                    rows.terms.push_back ({ { index (vertex), 1 }, { index (vertex + 5), -1 } });
                if (x == 0 || x == 2 || x == 4 || y == 0 || y == 4)
                    fixed.push_back ({ index (vertex), double (std::min (x, 4U - x)) });
            }
        rows.Finish ();
        GradingRequest system;
        system.unknowns = 25;
        system.rows = rows.rows;
        system.fixed = fixed;
        EditSparseSolveResult result;
        std::string error = "old";
        ASSERT_TRUE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error)) << error;
        EXPECT_TRUE (error.empty ());
        ASSERT_EQ (result.solution.values.size (), 25U);
        EXPECT_EQ (result.vertices, vertices);
        EXPECT_EQ (result.operation, kOperation);
        EXPECT_EQ (result.revision, fixture.doc.revision);
        EXPECT_EQ (result.sourceTerrainGuid, input.sourceTerrainGuid);
        EXPECT_EQ (result.inputProvenance, fixture.provenance);
        EXPECT_EQ (result.solution.factorizations, 1U);
        for (size_t i = 0; i < result.vertices.size (); ++i) {
            const uint32_t x = result.vertices[i] % 5;
            EXPECT_NEAR (result.solution.values[i], double (std::min (x, 4U - x)), 1e-12);
        }
    }
    EXPECT_EQ (fixture.points, original);
    EXPECT_EQ (fixture.doc.landscape, landscape);
}

TEST (EditSparseSolve, SubsetBoundsAndDifferencesUseTheCertifiedRustSolve)
{
    SolveFixture fixture;
    const auto input = fixture.Input ();
    Rows rows = Chain (5);
    const std::array<uint32_t, 5> vertices { 20, 21, 22, 23, 24 };
    const std::array<GradingFixed, 2> fixed { GradingFixed { 0, 0 }, GradingFixed { 4, 4 } };
    const std::array<GradingBound, 1> bounds { GradingBound { 2, -10, 1.5 } };
    const std::array<GradingDifference, 1> differences { GradingDifference { 4, 0, -5, 5 } };
    GradingRequest system;
    system.unknowns = 5;
    system.rows = rows.rows;
    system.fixed = fixed;
    system.bounds = bounds;
    system.differences = differences;
    EditSparseSolveResult result;
    std::string error;
    ASSERT_TRUE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error)) << error;
    EXPECT_EQ (result.vertices, (std::vector<uint32_t> { 20, 21, 22, 23, 24 }));
    EXPECT_NEAR (result.solution.values[1], 0.75, 1e-10);
    EXPECT_NEAR (result.solution.values[2], 1.5, 1e-10);
    EXPECT_NEAR (result.solution.values[3], 2.75, 1e-10);
    EXPECT_GE (result.solution.active, 1U);
}

TEST (EditSparseSolve, EnabledAuthoredControlsCannotBeIgnoredByACallerAssembledSystem)
{
    SolveFixture fixture;
    const auto input = fixture.Input ();
    const auto rows = Chain (3);
    const std::array<uint32_t, 3> vertices { 0, 1, 2 };
    const std::array<GradingFixed, 2> fixed { GradingFixed { 0, 1 }, GradingFixed { 2, 3 } };
    GradingRequest system;
    system.unknowns = 3;
    system.rows = rows.rows;
    system.fixed = fixed;
    std::string error;
    for (Id vertex : { Id (0), Id (12) }) {
        fixture.doc.landscape->terrainControls = { { 10, vertex, true } };
        auto result = Sentinel ();
        EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
        EXPECT_EQ (error.find ("TerrainControlTierUnsupported:"), 0U) << error;
        ExpectSentinel (result);
        fixture.doc.landscape->terrainControls.front ().enabled = false;
        ASSERT_TRUE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error)) << error;
        EXPECT_NEAR (result.solution.values.at (1), 2, 1e-12);
    }
}

TEST (EditSparseSolve, ContradictoryFixedValuesAndUnanchoredSystemsNeverPublish)
{
    SolveFixture fixture;
    const auto input = fixture.Input ();
    Rows rows = Chain (3);
    const std::array<uint32_t, 3> vertices { 0, 1, 2 };
    const std::array<GradingFixed, 2> fixed { GradingFixed { 0, 0 }, GradingFixed { 0, 1 } };
    GradingRequest system;
    system.unknowns = 3;
    system.rows = rows.rows;
    system.fixed = fixed;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    EXPECT_EQ (error.find ("SolveInconsistent:"), 0U) << error;
    ExpectSentinel (result);
    system.fixed = {};
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    EXPECT_EQ (error.find ("SolveUnderdetermined"), 0U) << error;
    ExpectSentinel (result);
    const std::array<GradingFixed, 2> same { GradingFixed { 0, 0 }, GradingFixed { 0, 0 } };
    system.fixed = same;
    ASSERT_TRUE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error)) << error;
    for (double value : result.solution.values)
        EXPECT_NEAR (value, 0, 1e-12);
}

TEST (EditSparseSolve, ExhaustedConstrainedBudgetDoesNotPublishAnUncertifiedIterate)
{
    SolveFixture fixture;
    const auto input = fixture.Input ();
    const std::array<uint32_t, 5> vertices { 0, 1, 2, 3, 4 };
    const std::array<GradingTerm, 5> terms { GradingTerm { 0, 1 }, GradingTerm { 1, 1 }, GradingTerm { 2, 1 },
                                             GradingTerm { 3, 1 }, GradingTerm { 4, 1 } };
    const std::array<double, 5> targets { -3, -2, 0, 2, 3 };
    std::vector<GradingRow> rows;
    std::vector<GradingBound> bounds;
    for (uint32_t i = 0; i < 5; ++i) {
        rows.push_back ({ std::span<const GradingTerm> (terms).subspan (i, 1), targets[i], 1 });
        bounds.push_back ({ i, -1, 1 });
    }
    const std::array<GradingDifference, 1> differences { GradingDifference {
        4, 0, -std::numeric_limits<double>::infinity (), 0.5 } };
    const std::array<double, 5> start {};
    GradingRequest system;
    system.unknowns = 5;
    system.rows = rows;
    system.bounds = bounds;
    system.differences = differences;
    system.start = start;
    system.limits.maxIterations = 1;
    auto result = Sentinel ();
    std::string error;
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    EXPECT_EQ (error.find ("SolveNotConverged"), 0U) << error;
    ExpectSentinel (result);
    EXPECT_EQ (start, (std::array<double, 5> {}));
    system.limits = {};
    ASSERT_TRUE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error)) << error;
}

TEST (EditSparseSolve, UnknownLocatorsAreRequiredUniqueAndInRange)
{
    SolveFixture fixture;
    const auto input = fixture.Input ();
    Rows rows = Chain (3);
    const std::array<GradingFixed, 1> fixed { GradingFixed { 0, 0 } };
    GradingRequest system;
    system.unknowns = 3;
    system.rows = rows.rows;
    system.fixed = fixed;
    auto result = Sentinel ();
    std::string error;
    for (const auto& vertices : { std::vector<uint32_t> {}, std::vector<uint32_t> { 0, 1 },
                                  std::vector<uint32_t> { 0, 0, 2 }, std::vector<uint32_t> { 0, 1, 25 } }) {
        EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
        EXPECT_EQ (error.find ("SolveShape:"), 0U) << error;
        ExpectSentinel (result);
    }
}

TEST (EditSparseSolve, StaleFramingInvalidGeometryAndUnattributableOperationsRefuse)
{
    SolveFixture fixture;
    auto input = fixture.Input ();
    const std::array<uint32_t, 1> vertices { 0 };
    const std::array<GradingFixed, 1> fixed { GradingFixed { 0, 0 } };
    GradingRequest system;
    system.unknowns = 1;
    system.fixed = fixed;
    auto result = Sentinel ();
    std::string error;
    ++input.revision;
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    --input.revision;
    fixture.provenance.expectedSession = { 2 };
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    fixture.provenance.expectedSession = fixture.doc.landscape->sessionUuid;
    fixture.provenance.table.mappings.pop_back ();
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    ExpectSentinel (result);
    fixture = SolveFixture {};
    input.mesh = fixture.Input ().mesh;
    fixture.triangles[0] = 99;
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    fixture.triangles[0] = 0;
    fixture.points[2] = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    fixture.points[2] = 0;
    for (OperationId invalid : { OperationId (0), kOperation + 1 })
        EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, invalid, vertices, system }, result, error));
    fixture.doc.landscape->operations[0].enabled = false;
    EXPECT_FALSE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error));
    ExpectSentinel (result);
}

TEST (EditSparseSolve, InputsMayBorrowThePriorResultUntilPublication)
{
    SolveFixture fixture;
    const auto input = fixture.Input ();
    Rows rows = Chain (3);
    const std::array<uint32_t, 3> vertices { 0, 1, 2 };
    const std::array<GradingFixed, 2> fixed { GradingFixed { 0, 0 }, GradingFixed { 2, 2 } };
    GradingRequest system;
    system.unknowns = 3;
    system.rows = rows.rows;
    system.fixed = fixed;
    EditSparseSolveResult result;
    std::string error;
    ASSERT_TRUE (SolveEditTerrain (fixture.doc, { input, kOperation, vertices, system }, result, error)) << error;
    const EditTerrainInput borrowed { input.mesh, result.inputProvenance, result.revision, result.sourceTerrainGuid };
    system.start = result.solution.values;
    const EditSparseSolveRequest request { borrowed, result.operation, result.vertices, system };
    ASSERT_TRUE (SolveEditTerrain (fixture.doc, request, result, error)) << error;
    EXPECT_EQ (result.vertices, (std::vector<uint32_t> { 0, 1, 2 }));
    EXPECT_NEAR (result.solution.values[1], 1, 1e-12);
    EXPECT_EQ (result.inputProvenance, fixture.provenance);
}
} // namespace
