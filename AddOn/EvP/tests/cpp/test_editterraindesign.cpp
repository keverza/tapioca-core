#include "Edit/EditTerrainDesign.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cmath>

namespace {
using namespace tapioca::geometry;
namespace edit = geomsrv::edit;
constexpr SourceId kBaseline = 0x20000000000001ULL;
constexpr ObjectId kOwner = kBaseline + 1;
const std::array<double, 12> kRegion { -10, -15, 0, 50, -15, 0, 50, 15, 0, -10, 15, 0 };

// A 4 m path from (0, 0) at 10 m to (40, 0) at 12 m over the Appendix A base:
// 17 x 13 vertices at 5 m on z = 10 + 0.05x - 0.2y, offset off the band's lines.
struct DesignFixture {
    edit::Document doc;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    ProvenanceRequest provenance;
    Id path = 0, source = 0, stroke = 0;

    DesignFixture ()
    {
        std::string error;
        EXPECT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
        path = edit::AddPath (doc, { { 0, 0, 10 }, { 40, 0, 12 } }, 4.0);
        EXPECT_TRUE (edit::EditPath (
            doc, path,
            [] (edit::Path& value, std::string&) {
                for (auto& section : value.sections) {
                    section.crossfall = 0;
                    section.cutSlope = section.fillSlope = 50; // 1:2
                }
                return true;
            },
            error))
            << error;
        LandscapeSource centreline;
        centreline.pathId = path;
        centreline.role = SourceRole::PathCenterline;
        EXPECT_TRUE (AddLandscapeSource (doc, centreline, source, error)) << error;
        LandscapeOperation operation;
        operation.kind = OperationKind::Stroke;
        operation.inputs = { source };
        operation.width = 4;
        EXPECT_TRUE (AddLandscapeOperation (doc, operation, stroke, error)) << error;
        doc.nextId = kOwner + 1;
        doc.landscape->sourceTerrainGuid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
        for (uint32_t row = 0; row < 13; ++row)
            for (uint32_t column = 0; column < 17; ++column) {
                const double x = -20.5 + 5.0 * column, y = -30.5 + 5.0 * row;
                points.insert (points.end (), { x, y, 10 + .05 * x - .2 * y });
            }
        for (uint32_t row = 0; row < 12; ++row)
            for (uint32_t column = 0; column < 16; ++column) {
                const uint32_t a = row * 17 + column;
                triangles.insert (triangles.end (), { a, a + 1, a + 18, a, a + 18, a + 17 });
            }
        Capture ();
    }
    void Capture ()
    {
        provenance = {};
        provenance.expectedSession = doc.landscape->sessionUuid;
        provenance.expectedRevision = doc.revision;
        provenance.counts = { points.size () / 3, 0, triangles.size () / 3, 0 };
        auto& table = provenance.table;
        table.context = { 1, provenance.expectedSession, doc.revision, 100 };
        table.sources = { { kBaseline, {} } };
        for (const auto& value : doc.landscape->sources)
            table.sources.push_back ({ value.id, {} });
        table.operations = { { stroke, {} } };
        table.operationIds = { stroke };
        table.objects = { { kOwner, stroke } };
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
        table.roleBytes = { 't', 'e', 'r', 'r', 'a', 'i', 'n' };
        table.bindings = { owner };
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
    TerrainDesignOptions Options () const
    {
        TerrainDesignOptions options;
        options.operation = stroke;
        options.object = kOwner;
        options.source = source;
        options.region = kRegion;
        options.maxStep = 5;
        options.tolerance = 1e-6;
        options.designTable = 101;
        options.meshTable = 102;
        return options;
    }
};

bool Contains (const std::vector<uint64_t>& pool, IdentityRange range, uint64_t id)
{
    return std::find (pool.begin () + range.offset, pool.begin () + range.offset + range.count, id) !=
           pool.begin () + range.offset + range.count;
}
} // namespace

TEST (EditTerrainDesign, PathBandBuildsWithAttributedMeshAndExactOutside)
{
    DesignFixture fixture;
    const auto original = fixture.provenance;
    const auto before = fixture.doc.landscape;
    TerrainDesignResult result;
    std::string error;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), result, error)) << error;
    EXPECT_EQ (fixture.provenance, original);
    EXPECT_EQ (fixture.doc.landscape, before);
    // Stations come from the path: every 5 m from 0 to 40, heights on its profile.
    ASSERT_EQ (result.stations.size (), 9U);
    EXPECT_EQ (result.stations.front (), 0.0);
    EXPECT_EQ (result.stations.back (), 40.0);
    const auto& build = result.build;
    EXPECT_EQ (build.inputProvenance.domains, 7U);
    EXPECT_EQ (build.inputProvenance.table.context.tableId, 101U);
    for (const auto& pair : build.sides)
        for (const auto& side : pair) {
            EXPECT_EQ (side.roots, 1U);
            EXPECT_NEAR (side.first, 4.0 / 3.0, 1e-9);
        }
    EXPECT_GE (build.site.fill, 80.0 / 3.0 - 1e-9);
    EXPECT_GE (build.site.cut, 80.0 / 3.0 - 1e-9);
    EXPECT_EQ (build.context.fill, 0.0);
    // The rebuilt mesh is fully attributed, vertex and face.
    const auto& mesh = result.meshProvenance;
    EXPECT_EQ (mesh.domains, kMeshProvenanceDomains);
    EXPECT_EQ (mesh.table.context.tableId, 102U);
    EXPECT_EQ (mesh.counts, (EntityCounts { build.points.size () / 3, 0, build.triangles.size () / 3, 0 }));
    EXPECT_EQ (mesh.table.bindings, original.table.bindings);
    size_t exact = 0, split = 0, generated = 0;
    for (const auto& row : mesh.table.mappings) {
        if (row.domain == IdentityDomain::Vertex && row.outputIndex < fixture.points.size () / 3)
            EXPECT_EQ (row.kind, MappingKind::Exact);
        exact += row.kind == MappingKind::Exact;
        split += row.kind == MappingKind::Split;
        generated += row.kind == MappingKind::Generated;
        if (row.kind != MappingKind::Exact) {
            EXPECT_TRUE (Contains (mesh.table.sourceIds, row.sources, fixture.source));
            EXPECT_TRUE (Contains (mesh.table.operationIds, row.operations, fixture.stroke));
        }
        EXPECT_EQ (row.object, kOwner);
    }
    EXPECT_GT (split, 0U);     // Ring and base-edge crossings, re-triangulated faces.
    EXPECT_GT (generated, 0U); // Band-line vertices the base never had.
    EXPECT_EQ (exact + split + generated, mesh.table.mappings.size ());
    // Heights the band moved are the operation's; outside vertices keep base bits.
    ASSERT_EQ (result.vertexOperations.size (), build.base.size ());
    size_t moved = 0;
    for (size_t v = 0; v < build.base.size (); ++v) {
        const double x = build.points[v * 3], y = build.points[v * 3 + 1];
        if (result.vertexOperations[v]) {
            ++moved;
            EXPECT_EQ (*result.vertexOperations[v], fixture.stroke);
        }
        if (x < -10 || x > 50 || std::abs (y) > 15)
            EXPECT_FALSE (result.vertexOperations[v].has_value ());
        if (x >= 0 && x <= 40 && std::abs (y) <= 2 + 1e-12)
            EXPECT_NEAR (build.points[v * 3 + 2], 10 + .05 * x, 1e-12);
    }
    EXPECT_GT (moved, 0U);
    TerrainDesignResult again;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), again, error)) << error;
    EXPECT_EQ (again, result);
}

TEST (EditTerrainDesign, RefusalsLeaveTheResultUnchanged)
{
    DesignFixture fixture;
    TerrainDesignResult result;
    result.stations = { 77 };
    const auto before = result;
    std::string error;
    auto expectRefusal = [&] (const edit::Document& doc, const EditTerrainInput& input,
                              const TerrainDesignOptions& options, const char* code) {
        EXPECT_FALSE (EvaluateTerrainDesign (doc, input, options, result, error));
        EXPECT_EQ (error.rfind (code, 0), 0U) << error;
        EXPECT_EQ (result, before);
    };
    auto options = fixture.Options ();
    options.meshTable = options.designTable;
    expectRefusal (fixture.doc, fixture.Input (), options, "ProvenanceConflict");
    options = fixture.Options ();
    options.source = fixture.path; // Not a landscape source.
    expectRefusal (fixture.doc, fixture.Input (), options, "ProvenanceUnattributable");
    options = fixture.Options ();
    options.fillLimit = options.cutLimit = .5;
    expectRefusal (fixture.doc, fixture.Input (), options, "DaylightNotReached");
    options = fixture.Options ();
    options.region = std::span<const double> (kRegion).first (6);
    expectRefusal (fixture.doc, fixture.Input (), options, "TerrainDesignRegion");
    options = fixture.Options ();
    options.maxStep = 0;
    expectRefusal (fixture.doc, fixture.Input (), options, "TerrainDesignParameters");
    // A stale capture refuses before any geometry.
    auto stale = fixture.Input ();
    stale.revision += 1;
    expectRefusal (fixture.doc, stale, fixture.Options (), "ProvenanceStale");
}

TEST (EditTerrainDesign, CrownedPathBuildsACentrelineCrease)
{
    DesignFixture fixture;
    std::string error;
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, fixture.path,
        [] (edit::Path& value, std::string&) {
            for (auto& section : value.sections) {
                section.crossfall = 2;
                section.fall = edit::Fall::Crown;
            }
            return true;
        },
        error))
        << error;
    fixture.Capture ();
    TerrainDesignResult result;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), result, error)) << error;
    const auto& build = result.build;
    EXPECT_EQ (std::count_if (build.lines.begin (), build.lines.end (),
                              [] (const auto& line) { return line.kind == HybridLineKind::Crown; }),
               1);
    // The downhill (left) edge fills to 1.2 m, the uphill one cuts to 22/15 m.
    for (const auto& pair : build.sides) {
        EXPECT_NEAR (pair[0].first, 1.2, 1e-9);
        EXPECT_NEAR (pair[1].first, 22.0 / 15.0, 1e-9);
    }
    EXPECT_GE (build.site.fill, 40 * .576 - 1e-9);
    EXPECT_GE (build.site.cut, 40 * (.44 + .5 * .44 * 22.0 / 15.0) - 1e-9);
    for (size_t v = 0; v < build.base.size (); ++v) {
        const double x = build.points[v * 3], y = build.points[v * 3 + 1];
        if (x >= 0 && x <= 40 && std::abs (y) <= 2 + 1e-12)
            EXPECT_NEAR (build.points[v * 3 + 2], 10 + .05 * x - .02 * std::abs (y), 1e-12);
    }
    EXPECT_EQ (result.meshProvenance.counts.vertices, build.points.size () / 3);
}

TEST (EditTerrainDesign, ARangeChangeIsAShortTaperNotARamp)
{
    DesignFixture fixture;
    std::string error;
    Id wide = 0;
    ASSERT_TRUE (edit::SplitPathSection (fixture.doc, fixture.path, 20, error, &wide)) << error;
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, fixture.path,
        [&] (edit::Path& value, std::string& why) { return edit::SetWidth (value.sections, wide, 3, 3, why); }, error))
        << error;
    fixture.Capture ();
    TerrainDesignResult result;
    auto options = fixture.Options ();
    options.taper = .5;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error)) << error;
    // Every 5 m plus the taper start 0.5 m before the range.
    const std::vector<double> expected { 0, 5, 10, 15, 19.5, 20, 25, 30, 35, 40 };
    EXPECT_EQ (result.stations, expected);
    // The left band edge is 2 m out up to the taper start and 3 m from the range on.
    const auto& build = result.build;
    const auto edge =
        std::find (build.lines.begin (), build.lines.end (), HybridLine { HybridLineKind::Edge, TerrainHand::Left, 0 });
    ASSERT_NE (edge, build.lines.end ());
    const auto line = static_cast<uint32_t> (edge - build.lines.begin ());
    size_t checked = 0;
    for (const auto& row : build.mappings) {
        if (row.kind != TerrainMappingKind::DesignLineVertex || row.index != line)
            continue;
        const double station = result.stations.at (row.values[0]);
        const double y = build.points.at (row.values[1] * 3 + 1);
        EXPECT_NEAR (y, station <= 19.5 ? 2.0 : 3.0, 1e-9) << station;
        ++checked;
    }
    EXPECT_EQ (checked, expected.size ());
    options.taper = 0;
    EXPECT_FALSE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error));
    EXPECT_EQ (error.rfind ("TerrainDesignParameters", 0), 0U) << error;
}

TEST (EditTerrainDesign, ConflictsNameStationsSidesAndPlaces)
{
    DesignFixture fixture;
    auto options = fixture.Options ();
    options.cutLimit = options.fillLimit = .5;
    TerrainDesignResult result;
    std::string error;
    EXPECT_FALSE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error));
    EXPECT_EQ (error.rfind ("DaylightNotReached", 0), 0U) << error;
    EXPECT_NE (error.find ("at station 0.00 m, left"), std::string::npos) << error;
    EXPECT_NE (error.find ("18 located"), std::string::npos) << error;
    std::vector<TerrainDesignConflict> conflicts;
    ASSERT_TRUE (AssessTerrainDesign (fixture.doc, fixture.Input (), options, conflicts, error)) << error;
    ASSERT_EQ (conflicts.size (), 18U); // Nine stations, both sides.
    EXPECT_EQ (conflicts.front ().station, 0.0);
    EXPECT_EQ (conflicts.back ().station, 40.0);
    EXPECT_EQ (conflicts.back ().side, TerrainHand::Right);
    for (const auto& conflict : conflicts) {
        EXPECT_EQ (conflict.code, HybridConflictCode::DaylightBeyondLimit);
        EXPECT_NEAR (conflict.magnitude, 4.0 / 3.0, 1e-9);
    }
    // Fill only: the right side is held at the ground at every station.
    options = fixture.Options ();
    options.effect = TerrainEditEffect::Fill;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error)) << error;
    ASSERT_EQ (result.conflicts.size (), 9U);
    for (const auto& conflict : result.conflicts) {
        EXPECT_EQ (conflict.code, HybridConflictCode::EdgeContradictsEffect);
        EXPECT_EQ (conflict.side, TerrainHand::Right);
        EXPECT_FALSE (conflict.hard);
    }
}

namespace {
// Plan distance from (x, y) to the fixture's centreline segment (0,0)-(40,0).
double FromCentre (double x, double y)
{
    const double t = std::clamp (x, 0.0, 40.0);
    return std::hypot (x - t, y);
}
} // namespace

TEST (EditTerrainDesign, AnEmptyRegionIsDerivedFromTheBandsReach)
{
    DesignFixture fixture;
    auto options = fixture.Options ();
    options.region = {};
    TerrainDesignResult result;
    std::string error;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error)) << error;
    // Reach = 2 m half width + 10 m limit + 1 m margin, round ends.
    ASSERT_GE (result.region.size (), 3 * 8U);
    for (size_t i = 0; i < result.region.size (); i += 3) {
        const double d = FromCentre (result.region[i], result.region[i + 1]);
        EXPECT_GT (d, 13 - options.arcTolerance - 1e-6);
        EXPECT_LT (d, 13 + 1e-3);
    }
    EXPECT_GE (result.build.site.fill, 80.0 / 3.0 - 1e-9);
    EXPECT_EQ (result.build.context.fill, 0.0);
    // The derived ring is attributed like a caller's: its vertices follow the input prefix.
    EXPECT_EQ (result.build.inputProvenance.counts.edges, result.region.size () / 3);
}

TEST (EditTerrainDesign, TheDerivedRegionIsClippedToThePropertyLine)
{
    DesignFixture fixture;
    std::string error;
    auto property = [&] (std::vector<Vec3> ring) {
        LandscapeSource source;
        source.entityId = edit::AddPolyline (fixture.doc, ring, true);
        Id id = 0;
        EXPECT_TRUE (AddLandscapeSource (fixture.doc, source, id, error)) << error;
        EXPECT_TRUE (SetLandscapePropertyLine (fixture.doc, id, error)) << error;
        fixture.Capture ();
    };
    property ({ { -5, -8, 0 }, { 45, -8, 0 }, { 45, 8, 0 }, { -5, 8, 0 } });
    auto options = fixture.Options ();
    options.region = {};
    TerrainDesignResult result;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error)) << error;
    for (size_t i = 0; i < result.region.size (); i += 3) {
        EXPECT_GE (result.region[i], -5 - 1e-6);
        EXPECT_LE (result.region[i], 45 + 1e-6);
        EXPECT_LE (std::abs (result.region[i + 1]), 8 + 1e-6);
    }
    EXPECT_GE (result.build.site.fill, 80.0 / 3.0 - 1e-9);
    // A property line the band never reaches leaves no region.
    DesignFixture far;
    fixture = std::move (far);
    property ({ { 100, 100, 0 }, { 110, 100, 0 }, { 110, 110, 0 }, { 100, 110, 0 } });
    TerrainDesignResult untouched;
    EXPECT_FALSE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, untouched, error));
    EXPECT_EQ (error.rfind ("TerrainDesignRegion", 0), 0U) << error;
    EXPECT_TRUE (untouched.region.empty ());
}
