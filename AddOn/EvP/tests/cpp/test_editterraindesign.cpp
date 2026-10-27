#include "Edit/EditTerrainDesign.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditTerrainOutline.hpp"
#include "Geometry/Mesh.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPathEdit.hpp"
#include "archviz_log_fixture.hpp"
#include "Edit/EditPathNetwork.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <thread>
#include <chrono>

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
                    section.rounding = 0;                      // the worked volumes are the sharp ones
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

TEST (EditTerrainDesign, DenseStationsOnTheWorkedGridKeepExactTouchingVertices)
{
    DesignFixture fixture;
    std::string error;
    for (bool derived : { false, true }) {
        auto options = fixture.Options ();
        options.maxStep = 2;
        if (derived)
            options.region = {};
        TerrainDesignResult result;
        ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error)) << error;
        EXPECT_EQ (result.stations.size (), 21U);
        EXPECT_EQ (result.build.context.cut, 0);
        EXPECT_EQ (result.build.context.fill, 0);
        // Every station at y=2 which lies exactly on a grid diagonal remains
        // a design vertex even when another crossing rounds off that diagonal.
        for (double x : { 2.0, 12.0, 22.0, 32.0 }) {
            bool found = false;
            for (size_t i = 0; i < result.build.points.size (); i += 3)
                if (result.build.points[i] == x && result.build.points[i + 1] == 2) {
                    EXPECT_NEAR (result.build.points[i + 2], 10 + .05 * x, 1e-12);
                    found = true;
                }
            EXPECT_TRUE (found) << x;
        }
    }
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

TEST (EditTerrainDesign, TaperedSectionWidthsReachTerrainStationsAndDaylights)
{
    DesignFixture fixture;
    std::string error;
    const auto range = edit::FindPath (fixture.doc, fixture.path)->sections.front ().id;
    ASSERT_TRUE (edit::SetPathTaper (fixture.doc, fixture.path, range, true, 4.0, 1.0, error)) << error;
    fixture.Capture ();
    TerrainDesignResult result;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), result, error)) << error;
    const std::vector<double> expected { 0, 5, 10, 15, 20, 25, 30, 35, 40 };
    ASSERT_EQ (result.stations, expected);
    const auto& build = result.build;
    ASSERT_EQ (build.sides.size (), result.stations.size ());
    const std::array<TerrainHand, 2> hands { TerrainHand::Left, TerrainHand::Right };
    std::array<size_t, 2> lines { build.lines.size (), build.lines.size () };
    for (size_t side = 0; side < 2; ++side) {
        const auto line =
            std::find (build.lines.begin (), build.lines.end (), HybridLine { HybridLineKind::Edge, hands[side], 0 });
        ASSERT_NE (line, build.lines.end ());
        lines[side] = static_cast<size_t> (line - build.lines.begin ());
    }
    // A 2 m half width tapers to 4 m on the left and 1 m on the right over 40 m.
    // The daylight root follows the edge's own height difference: 0.2w / (0.5 - 0.2).
    std::array<size_t, 2> checked {};
    for (size_t station = 0; station < result.stations.size (); ++station) {
        const double t = result.stations[station] / 40.0;
        const std::array<double, 2> widths { 2.0 + 2.0 * t, 2.0 - t };
        ASSERT_EQ (build.sides[station].size (), 2U);
        for (size_t side = 0; side < 2; ++side) {
            EXPECT_EQ (build.sides[station][side].roots, 1U);
            EXPECT_NEAR (build.sides[station][side].first, widths[side] * 2.0 / 3.0, 1e-9)
                << result.stations[station] << " side " << side;
        }
    }
    for (const auto& row : build.mappings) {
        if (row.kind != TerrainMappingKind::DesignLineVertex)
            continue;
        for (size_t side = 0; side < 2; ++side) {
            if (row.index != lines[side])
                continue;
            const double t = result.stations.at (row.values[0]) / 40.0;
            const double width = side == 0 ? 2.0 + 2.0 * t : 2.0 - t;
            const double y = build.points.at (row.values[1] * 3 + 1);
            EXPECT_NEAR (y, side == 0 ? width : -width, 1e-9) << result.stations.at (row.values[0]);
            EXPECT_NEAR (build.points.at (row.values[1] * 3 + 2), 10.0 + 2.0 * t, 1e-9);
            ++checked[side];
        }
    }
    EXPECT_EQ (checked[0], result.stations.size ());
    EXPECT_EQ (checked[1], result.stations.size ());
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

TEST (EditTerrainDesign, ARoundedSectionLinesItsFacesAndEasesTheirVolume)
{
    DesignFixture fixture;
    std::string error;
    TerrainDesignResult sharp, rounded;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), sharp, error)) << error;
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, fixture.path,
        [] (edit::Path& value, std::string&) {
            for (auto& section : value.sections)
                section.rounding = 1.0;
            return true;
        },
        error))
        << error;
    fixture.Capture ();
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), rounded, error)) << error;
    const auto faces = [] (const TerrainDesignResult& result) {
        return std::count_if (result.build.lines.begin (), result.build.lines.end (),
                              [] (const auto& line) { return line.kind == HybridLineKind::Face; });
    };
    EXPECT_EQ (faces (sharp), 0);
    EXPECT_EQ (faces (rounded), 16) << "eight lines across each side's face";
    // Rounding takes the corners off both the cut and the fill.
    EXPECT_LT (rounded.build.site.fill, sharp.build.site.fill);
    EXPECT_LT (rounded.build.site.cut, sharp.build.site.cut);
    EXPECT_GT (rounded.build.site.fill, 0.8 * sharp.build.site.fill);
}

TEST (EditTerrainDesign, ACoverClipsTheDerivedRegionToTheTerrainsFootprint)
{
    DesignFixture fixture;
    // The fixture's mesh spans x -20.5..59.5, y -30.5..29.5; its footprint is inset 1 cm.
    const auto footprint = TerrainFootprint ({ fixture.points, fixture.triangles });
    ASSERT_GE (footprint.size (), 12U);
    for (size_t i = 0; i < footprint.size (); i += 3) {
        EXPECT_GE (footprint[i], -20.5 + 0.01 - 1e-6);
        EXPECT_LE (footprint[i + 1], 29.5 - 0.01 + 1e-6);
    }
    // A path starting 2 m inside the mesh's left edge: its 13 m reach leaves the mesh.
    std::string error;
    ASSERT_TRUE (
        edit::MovePathPoint (fixture.doc, fixture.path, fixture.doc.paths[0].points[0].id, { -18, 0, 0 }, error))
        << error;
    fixture.Capture ();
    auto options = fixture.Options ();
    options.region = {};
    TerrainDesignResult result;
    EXPECT_FALSE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error));
    options.cover = footprint;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error)) << error;
    for (size_t i = 0; i < result.region.size (); i += 3)
        EXPECT_GE (result.region[i], -20.5);
    // Past the edge, the band itself is off the terrain: trimmed, the part over it is graded.
    ASSERT_TRUE (
        edit::MovePathPoint (fixture.doc, fixture.path, fixture.doc.paths[0].points[0].id, { -30, 0, 0 }, error))
        << error;
    fixture.Capture ();
    EXPECT_FALSE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error));
    EXPECT_EQ (error.rfind ("DesignCoverage", 0), 0U) << error;
    options.trimToCover = true;
    ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, result, error)) << error;
    EXPECT_GT (result.stations.front (), 9.0) << "stations off the terrain are dropped";
}

namespace {
std::shared_ptr<const TerrainCapture> Capture (const DesignFixture& fixture)
{
    auto capture = std::make_shared<TerrainCapture> ();
    capture->points = fixture.points;
    capture->triangles = fixture.triangles;
    capture->provenance = fixture.provenance;
    capture->revision = fixture.doc.revision;
    capture->sourceTerrainGuid = fixture.doc.landscape->sourceTerrainGuid;
    return capture;
}
} // namespace

TEST (EditTerrainDesignJob, APathMayLeaveTheTerrainAndThePropertyLineAndIsGradedOnlyInside)
{
    DesignFixture fixture;
    std::string error;
    // Off the terrain at its west end (the mesh starts at x = -20.5) ...
    ASSERT_TRUE (
        edit::MovePathPoint (fixture.doc, fixture.path, fixture.doc.paths[0].points[0].id, { -30, 0, 0 }, error))
        << error;
    // ... and past a property line that stops at x = 30.
    LandscapeSource property;
    property.entityId =
        edit::AddPolyline (fixture.doc, { { -40, -20, 0 }, { 30, -20, 0 }, { 30, 20, 0 }, { -40, 20, 0 } }, true);
    Id id = 0;
    ASSERT_TRUE (AddLandscapeSource (fixture.doc, property, id, error)) << error;
    ASSERT_TRUE (SetLandscapePropertyLine (fixture.doc, id, error)) << error;
    fixture.Capture ();
    TerrainDesignPublisher publisher;
    TerrainDesignJob job;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), fixture.Options (), {}, job, error)) << error;
    const auto outcome = RunTerrainDesign (job);
    ASSERT_TRUE (outcome.built) << outcome.error;
    EXPECT_TRUE (outcome.unmet.empty ()) << "leaving them is allowed, not a condition missed";
    // Nothing is graded outside: the region and its stations stay inside both.
    for (size_t i = 0; i < outcome.result.region.size (); i += 3) {
        EXPECT_GE (outcome.result.region[i], -20.5);
        EXPECT_LE (outcome.result.region[i], 30.0);
    }
    EXPECT_GT (outcome.result.stations.front (), 9.0);
}

TEST (EditTerrainDesignJob, OutlineGradingShapesTheTerrainFromEveryPath)
{
    DesignFixture fixture;
    std::string error;
    // A second path crossing the first, raised 1.5 m above the ground there.
    const auto second = edit::AddPath (fixture.doc, { { 20, -12, 13.5 }, { 20, 12, 13.5 } }, 3.0);
    ASSERT_NE (second, edit::kNoId);
    fixture.Capture ();
    TerrainDesignPublisher publisher;
    TerrainDesignJob job;
    auto options = fixture.Options ();
    options.region = {};
    options.outline = true;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, {}, job, error)) << error;
    const auto outcome = RunTerrainDesign (job);
    ASSERT_TRUE (outcome.built) << outcome.error;
    const auto& build = outcome.result.build;
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    // On each path the terrain is that path's paved surface.
    const std::array<double, 4> queries { 5, 0, 20, -8 };
    ASSERT_TRUE (SampleTerrain ({ build.points, build.triangles }, queries, .25, samples, summary, error)) << error;
    double paved = 0;
    const auto paths = edit::Pave (fixture.doc.paths);
    ASSERT_TRUE (edit::PavedHeight (paths, { 5, 0, 0 }, paved));
    EXPECT_NEAR (samples[0].height, paved, 0.05);
    ASSERT_TRUE (edit::PavedHeight (paths, { 20, -8, 0 }, paved));
    EXPECT_NEAR (samples[1].height, paved, 0.05) << "the second path shapes the terrain too";
    EXPECT_GT (build.site.fill, 0.0);
    // The region ring is published for drawing.
    EXPECT_FALSE (outcome.result.region.empty ());
    EXPECT_TRUE (std::any_of (build.mappings.begin (), build.mappings.end (),
                              [] (const auto& row) { return row.kind == TerrainMappingKind::RingVertex; }));
}

TEST (EditTerrainDesignJob, AWorkerResultPublishesOnlyForTheDocumentItWasComputedAgainst)
{
    DesignFixture fixture;
    TerrainDesignPublisher publisher;
    TerrainDesignJob job;
    std::string error;
    auto options = fixture.Options ();
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, {}, job, error)) << error;
    EXPECT_EQ (job.generation, 1U);
    // The worker runs off the main thread against its own snapshot.
    auto outcome = LaunchTerrainDesign (job).get ();
    ASSERT_TRUE (outcome.built) << outcome.error;
    EXPECT_EQ (outcome.revision, fixture.doc.revision);
    EXPECT_FALSE (outcome.result.region.empty ()); // Derived reach.
    const auto copy = outcome;
    ASSERT_TRUE (publisher.Publish (fixture.doc, std::move (outcome), error)) << error;
    const auto* current = publisher.Current (fixture.doc);
    ASSERT_NE (current, nullptr);
    EXPECT_EQ (current->result, copy.result);
    EXPECT_EQ (current->result, RunTerrainDesign (job).result); // Deterministic.

    // An edit moves the revision: the cached result no longer describes it, and
    // a result computed against the old revision is refused.
    auto edited = fixture.doc;
    ASSERT_TRUE (edit::EditPath (
        edited, fixture.path,
        [] (edit::Path& value, std::string&) {
            value.sections.front ().widthLeft = 2.5;
            return true;
        },
        error))
        << error;
    ++edited.revision; // The session publishes an accepted edit as a new revision.
    EXPECT_EQ (publisher.Current (edited), nullptr);
    EXPECT_FALSE (publisher.Publish (edited, copy, error));
    EXPECT_EQ (error.rfind ("ProvenanceStale", 0), 0U) << error;
    // A capture stamped for the old revision cannot start a job for the new one.
    TerrainDesignJob stale;
    EXPECT_FALSE (publisher.Prepare (edited, Capture (fixture), options, {}, stale, error));
    EXPECT_EQ (error.rfind ("ProvenanceStale", 0), 0U) << error;
    EXPECT_EQ (publisher.Latest (), 1U);
}

TEST (EditTerrainDesignJob, LaterRequestsSupersedeAndRefusalsCarryTheirPlaces)
{
    DesignFixture fixture;
    TerrainDesignPublisher publisher;
    std::string error;
    TerrainDesignJob first, second;
    auto options = fixture.Options ();
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, kRegion, first, error)) << error;
    options.cutLimit = options.fillLimit = .5;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, kRegion, second, error)) << error;
    auto late = std::async (std::launch::async, [first] { return RunTerrainDesign (first); });
    auto relaxed = RunTerrainDesign (second);
    // The first job finishes, but it was superseded: discarded, nothing published.
    EXPECT_FALSE (publisher.Publish (fixture.doc, late.get (), error));
    EXPECT_EQ (error.rfind ("TerrainDesignSuperseded", 0), 0U) << error;
    EXPECT_EQ (publisher.Current (fixture.doc), nullptr);
    // The latest's slopes do not reach the ground within 0.5 m: the worker builds anyway with a
    // wider reach, and says so.
    ASSERT_TRUE (relaxed.built) << relaxed.error;
    ASSERT_EQ (relaxed.unmet.size (), 1U);
    EXPECT_EQ (relaxed.unmet[0].rfind ("The side slopes do not meet the ground within 0.5 m", 0), 0U)
        << relaxed.unmet[0];
    ASSERT_TRUE (publisher.Publish (fixture.doc, relaxed, error)) << error;
    const auto* current = publisher.Current (fixture.doc);
    ASSERT_NE (current, nullptr);
    EXPECT_TRUE (current->built);
    // A job without a snapshot is an outcome, not a crash.
    EXPECT_FALSE (RunTerrainDesign (TerrainDesignJob {}).built);
}

TEST (EditTerrainCapture, ASnapshotMeshBecomesAValidatedCaptureTheWorkerCanBuildOn)
{
    DesignFixture fixture;
    std::string error;
    Id terrain = 0, owner = 0;
    const std::string guid = "9bb0abd8-b1f3-4d78-855b-9106fd753bda";
    ASSERT_TRUE (SetLandscapeTerrain (fixture.doc, guid, terrain, error)) << error;
    HardscapeObject object;
    object.kind = ObjectKind::Path;
    object.pathId = fixture.path;
    object.footprintOperation = fixture.stroke;
    object.name = "path";
    ASSERT_TRUE (AddHardscapeObject (fixture.doc, object, owner, error)) << error;
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    mesh.vertices = fixture.points;
    mesh.triangles = fixture.triangles;
    TerrainCapture capture;
    ASSERT_TRUE (CaptureLandscapeTerrain (fixture.doc, mesh, { owner, 300, 290 }, capture, error)) << error;
    EXPECT_EQ (capture.revision, fixture.doc.revision);
    EXPECT_EQ (capture.sourceTerrainGuid, fixture.doc.landscape->sourceTerrainGuid);
    EXPECT_EQ (capture.provenance.counts,
               (EntityCounts { mesh.vertices.size () / 3, 0, mesh.triangles.size () / 3, 0 }));
    // Every captured row cites the terrain source and its owner.
    const auto& table = capture.provenance.table;
    for (const auto& row : table.mappings) {
        ASSERT_EQ (row.sources.count, 1U);
        EXPECT_EQ (table.sourceIds.at (row.sources.offset), terrain);
        EXPECT_EQ (row.object, owner);
        EXPECT_EQ (row.kind, MappingKind::Exact);
    }

    // Main thread prepares, a worker builds, the main thread publishes.
    TerrainDesignPublisher publisher;
    TerrainDesignJob job;
    auto options = fixture.Options ();
    options.object = owner;
    ASSERT_TRUE (
        publisher.Prepare (fixture.doc, std::make_shared<const TerrainCapture> (capture), options, {}, job, error))
        << error;
    auto outcome = LaunchTerrainDesign (job).get ();
    ASSERT_TRUE (outcome.built) << outcome.error;
    EXPECT_GE (outcome.result.build.site.fill, 80.0 / 3.0 - 1e-9);
    EXPECT_EQ (outcome.result.meshProvenance.table.objects.front ().id, owner);
    ASSERT_TRUE (publisher.Publish (fixture.doc, std::move (outcome), error)) << error;
    EXPECT_NE (publisher.Current (fixture.doc), nullptr);

    // Refusals leave the capture as it was.
    const auto before = capture.provenance;
    auto other = mesh;
    other.guid = "AABBCCDD-1122-3344-5566-778899AABBCC";
    EXPECT_FALSE (CaptureLandscapeTerrain (fixture.doc, other, { owner, 300, 290 }, capture, error));
    EXPECT_EQ (error.rfind ("ProvenanceStale", 0), 0U) << error;
    EXPECT_FALSE (CaptureLandscapeTerrain (fixture.doc, mesh, { owner + 1000, 300, 290 }, capture, error));
    EXPECT_EQ (error.rfind ("ProvenanceUnattributable", 0), 0U) << error;
    EXPECT_FALSE (CaptureLandscapeTerrain (fixture.doc, mesh, { owner, 300, 300 }, capture, error));
    auto broken = mesh;
    broken.vertices[2] = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (CaptureLandscapeTerrain (fixture.doc, broken, { owner, 300, 290 }, capture, error));
    EXPECT_EQ (error.rfind ("TerrainShape", 0), 0U) << error;
    EXPECT_EQ (capture.provenance, before);
}

TEST (EditTerrainCapture, NativeSolidSurfaceKeepsMergedAndFaceAncestryOnTheWorker)
{
    DesignFixture fixture;
    std::string error;
    Id terrain = 0, owner = 0;
    ASSERT_TRUE (SetLandscapeTerrain (fixture.doc, fixture.doc.landscape->sourceTerrainGuid, terrain, error));
    HardscapeObject object;
    object.kind = ObjectKind::Path;
    object.pathId = fixture.path;
    object.footprintOperation = fixture.stroke;
    ASSERT_TRUE (AddHardscapeObject (fixture.doc, object, owner, error)) << error;
    geomsrv::Mesh native;
    native.guid = fixture.doc.landscape->sourceTerrainGuid;
    native.vertices = fixture.points;
    native.triangles = fixture.triangles;
    const uint32_t duplicate = static_cast<uint32_t> (native.VertexCount ());
    native.vertices.insert (native.vertices.end (), fixture.points.begin (), fixture.points.begin () + 3);
    native.triangles[0] = duplicate;
    native.triangles.insert (native.triangles.end (), { 0, 18, 1 }); // downward native face
    auto capture = std::make_shared<TerrainCapture> ();
    ASSERT_TRUE (CaptureLandscapeTerrain (fixture.doc, native, { owner, 200, 100 }, *capture, error)) << error;
    TerrainCapture surface;
    ASSERT_TRUE (PrepareTerrainSurface (capture, 300, surface, error)) << error;
    EXPECT_EQ (surface.native, capture);
    EXPECT_EQ (surface.points, fixture.points);
    EXPECT_EQ (surface.triangles, fixture.triangles);
    EXPECT_EQ (surface.provenance.table.context.tableId, 300U);
    bool merged = false;
    for (const auto& row : surface.provenance.table.mappings) {
        EXPECT_EQ (row.object, owner);
        if (row.domain == IdentityDomain::Vertex && row.outputIndex == 0) {
            EXPECT_EQ (row.kind, MappingKind::Merged);
            ASSERT_EQ (row.predecessors.count, 2U);
            const auto& first = surface.provenance.table.predecessors.at (row.predecessors.offset);
            const auto& second = surface.provenance.table.predecessors.at (row.predecessors.offset + 1);
            EXPECT_EQ (first.tableId, 200U);
            EXPECT_EQ (first.entityIndex, 0U);
            EXPECT_EQ (second.entityIndex, duplicate);
            merged = true;
        }
    }
    EXPECT_TRUE (merged);
    const auto before = surface.provenance;
    EXPECT_FALSE (PrepareTerrainSurface (capture, 100, surface, error));
    EXPECT_EQ (surface.provenance, before);
    auto options = fixture.Options ();
    options.object = owner;
    TerrainDesignPublisher publisher;
    TerrainDesignJob job;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, capture, options, kRegion, job, error));
    job.surfaceTable = 300;
    auto outcome = LaunchTerrainDesign (job).get ();
    ASSERT_TRUE (outcome.built) << outcome.error;
    ASSERT_NE (outcome.capture, nullptr);
    EXPECT_EQ (outcome.capture->native, capture);
    EXPECT_EQ (outcome.result.build.inputProvenance.table.supportTables.back ().context.tableId, 300U);
}

namespace {
namespace eh = geomsrv::archviz::edithud;
namespace layers = geomsrv::archviz::overlaylayers;

struct TerrainHostFixture : DesignFixture {
    std::shared_ptr<const geomsrv::Mesh> mesh;
    TerrainHostFixture ()
    {
        std::string error;
        // Keep the controller fixture flat along the path. The twisted Appendix
        // A mesh above covers the kernel separately at its explicit 5 m spacing.
        EXPECT_TRUE (edit::EditPath (
            doc, path,
            [] (edit::Path& value, std::string&) {
                for (auto& point : value.profile)
                    point.z = 10;
                return true;
            },
            error))
            << error;
        LandscapeSource property;
        property.entityId =
            edit::AddPolyline (doc, { { -15, -20, 0 }, { 55, -20, 0 }, { 55, 20, 0 }, { -15, 20, 0 } }, true);
        Id id = 0;
        EXPECT_TRUE (AddLandscapeSource (doc, property, id, error)) << error;
        EXPECT_TRUE (SetLandscapePropertyLine (doc, id, error)) << error;
        auto base = std::make_shared<geomsrv::Mesh> ();
        base->guid = doc.landscape->sourceTerrainGuid;
        base->vertices = { -20.5, -30.5, 16.1, 59.5, -30.5, 16.1, 59.5, 29.5, 4.1, -20.5, 29.5, 4.1 };
        base->triangles = { 0, 1, 2, 0, 2, 3 };
        mesh = base;
    }
};

const layers::Mesh* TerrainMesh (const layers::Layer& layer)
{
    for (const auto& mesh : layer.meshes)
        if (mesh.rgba == 0x6E9A72FFu)
            return &mesh;
    return nullptr;
}

bool AwaitTerrain (eh::Session& session, layers::Layer& layer, bool refused = false)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        eh::Follow (session, layer);
        const auto* current = eh::CurrentTerrainDesign (session);
        if (current && (refused ? !current->built : current->built))
            return true;
        if (current && !refused && !current->built) {
            ADD_FAILURE () << current->error;
            return false;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}
} // namespace

TEST (HudTerrainDesign, LifecycleLogsDistinguishCaptureBuildPublicationAndLayerWithoutPerFrameSpam)
{
    geomsrv::archviz::test::LogCapture log;
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainPath = fixture.path;
    ASSERT_TRUE (eh::RequestTerrainCapture (session, true));
    session.terrainCapture.reset (); // Native bridge is exercised only by the SDK host.
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path));
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto contains = [&] (const std::string& text) {
        return std::any_of (log.lines.begin (), log.lines.end (),
                            [&] (const auto& line) { return line.find (text) != std::string::npos; });
    };
    EXPECT_TRUE (contains ("TERRAIN capture requested"));
    EXPECT_TRUE (contains ("TERRAIN start accepted"));
    EXPECT_TRUE (contains ("inputVertices=4 inputTriangles=2"));
    EXPECT_TRUE (contains ("TERRAIN worker launch"));
    EXPECT_TRUE (contains ("TERRAIN worker completed"));
    EXPECT_TRUE (contains ("built=1"));
    EXPECT_TRUE (contains ("TERRAIN publication accepted"));
    EXPECT_TRUE (contains ("TERRAIN layer proposed"));
    const auto count = log.lines.size ();
    for (int i = 0; i < 20; ++i)
        eh::Follow (session, layer);
    EXPECT_EQ (log.lines.size (), count);
    const auto* terrain = TerrainMesh (layer);
    ASSERT_NE (terrain, nullptr);
    EXPECT_TRUE (layers::DrawnByGuest (*terrain, layer)); // Existing Fade layer already uses the guest renderer.
}

TEST (HudTerrainDesign, CaptureUIRequestIsExplicitRevisionBoundAndDoesNotAuthorHistory)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainPath = fixture.path;
    const auto doc = session.edits.Doc ();
    ASSERT_TRUE (eh::RequestTerrainCapture (session, true)) << session.message;
    const auto request = session.terrainCapture;
    ASSERT_TRUE (request);
    EXPECT_EQ (request->path, fixture.path);
    EXPECT_TRUE (request->terrainGuid.empty ());
    EXPECT_TRUE (eh::TerrainCaptureIsCurrent (session, request));
    EXPECT_EQ (session.edits.Doc ().landscape, doc.landscape);
    EXPECT_EQ (session.edits.Doc ().entities, doc.entities);
    EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
    EXPECT_EQ (session.edits.Doc ().revision, doc.revision);
    EXPECT_EQ (session.edits.Doc ().nextId, doc.nextId);
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
    EXPECT_FALSE (eh::RequestTerrainCapture (session, true));
    EXPECT_EQ (session.terrainCapture, request);
    ASSERT_TRUE (session.edits.Apply ([=] (edit::Document& value, std::string&) {
        edit::FindPath (value, fixture.path)->profile.front ().z += 1;
        return true;
    }));
    EXPECT_FALSE (eh::TerrainCaptureIsCurrent (session, request));
    session.terrainCapture.reset ();
    ASSERT_TRUE (eh::RequestTerrainCapture (session, true));
    EXPECT_FALSE (eh::TerrainCaptureIsCurrent (session, request)); // An old queued action cannot consume the new one.
    EXPECT_TRUE (eh::TerrainCaptureIsCurrent (session, session.terrainCapture));
    session.close = true;
    EXPECT_FALSE (eh::TerrainCaptureIsCurrent (session, session.terrainCapture));
}

TEST (HudTerrainDesign, CaptureUIRequiresIdleInputButNeitherAPathNorAPropertyLine)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    // A stale explicit path is not retargeted: the terrain is set alone.
    session.terrainPath = fixture.doc.nextId;
    ASSERT_TRUE (eh::RequestTerrainCapture (session, true)) << session.message;
    EXPECT_EQ (session.terrainCapture->path, edit::kNoId);
    session.terrainCapture.reset ();
    session.terrainPath = fixture.path;
    ASSERT_TRUE (session.edits.Begin ());
    EXPECT_FALSE (eh::RequestTerrainCapture (session, true));
    session.edits.Cancel ();
    session.landscapeCommand.emplace ();
    EXPECT_FALSE (eh::RequestTerrainCapture (session, true));
    session.landscapeCommand.reset ();
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto depth = session.edits.UndoDepth ();
    ASSERT_TRUE (eh::RequestTerrainCapture (session, false));
    ASSERT_TRUE (session.terrainCapture);
    EXPECT_EQ (session.terrainCapture->terrainGuid, fixture.mesh->guid);
    EXPECT_EQ (session.edits.UndoDepth (), depth);
    session.terrainCapture.reset ();
    // The terrain first: no property line and no path needed to set it.
    auto bare = fixture.doc;
    bare.landscape->propertyLineSourceId = 0;
    eh::Session other (bare);
    ASSERT_TRUE (eh::RequestTerrainCapture (other, true)) << other.message;
    EXPECT_TRUE (other.terrainCapture);
}

TEST (HudTerrainDesign, ATerrainSetBeforeItsPathWaitsAndTheDesignStartsWhenOneIsChosen)
{
    TerrainHostFixture fixture;
    fixture.doc.landscape->propertyLineSourceId = 0;
    eh::Session session (fixture.doc);
    session.terrainPath = fixture.doc.nextId; // unresolvable: nothing to grade yet
    ASSERT_TRUE (eh::SetTerrainBaseline (session, eh::TerrainDesignBaseline { fixture.mesh, false, {} }, 0))
        << session.message;
    EXPECT_TRUE (session.terrainBaseline);
    EXPECT_FALSE (session.terrainDesign);
    EXPECT_EQ (eh::TerrainGuid (session), fixture.mesh->guid);
    EXPECT_EQ (session.edits.UndoDepth (), 0U) << "setting the terrain authors nothing";
    eh::PollTerrainDesign (session);
    EXPECT_TRUE (session.terrainBaseline) << "still waiting";
    ASSERT_TRUE (eh::ChooseTerrainDesignPath (session, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer)) << session.message;
    EXPECT_FALSE (session.terrainBaseline) << "the design owns it now";
    ASSERT_NE (eh::CurrentTerrainDesign (session), nullptr);
    EXPECT_TRUE (eh::CurrentTerrainDesign (session)->built) << eh::CurrentTerrainDesign (session)->error;
}

TEST (EditTerrainOutline, SmallPathMovesKeepSlopesLocalOnATwoTriangleTerrain)
{
    const std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
    const ProvenanceRequest provenance;
    for (double crease : { 0.0, 0.8 })
        for (double move : { -0.02, 0.0, 0.02 }) {
            const std::vector<double> points { 0, 0, 0, 40, 0, crease, 40, 40, crease, 0, 40, -crease };
            edit::Document doc;
            std::string error;
            ASSERT_TRUE (StartLandscape (doc, { 1 }, error));
            const auto path =
                edit::AddPath (doc, { { 5, 30, 0.36 }, { 14 + move, 20, 0.3 }, { 44, 25 + move, 0.16 } }, 3);
            edit::AlignmentGeometry geometry;
            ASSERT_TRUE (edit::BuildAlignment (doc.paths[0].points, geometry, error));
            edit::Id grade = 0;
            ASSERT_TRUE (edit::InsertPathBreak (doc, path, geometry.pointStations[1], error, &grade));
            ASSERT_TRUE (edit::SetPathElevation (doc, path, grade, 0.3, error));
            EditTerrainInput input { { points, triangles }, provenance, doc.revision, {} };
            TerrainDesignResult result;
            uint32_t unreached = 0;
            ASSERT_TRUE (EvaluateOutlineDesign (doc, input, {}, result, unreached, error)) << error;
            EXPECT_EQ (unreached, 0u) << "move " << move;
            const std::vector<double> queries { 5, 5, 30, 35, 37, 6, 25, 2 };
            std::vector<TerrainSample> samples, baseline;
            TerrainFieldSummary summary;
            ASSERT_TRUE (
                SampleTerrain ({ result.build.points, result.build.triangles }, queries, 0.25, samples, summary, error))
                << error;
            ASSERT_TRUE (SampleTerrain ({ points, triangles }, queries, 0.25, baseline, summary, error));
            ASSERT_EQ (samples.size (), baseline.size ());
            for (size_t i = 0; i < samples.size (); ++i) {
                EXPECT_TRUE (samples[i].onSurface);
                EXPECT_NEAR (samples[i].height, baseline[i].height, 1e-6)
                    << "remote terrain changed after move " << move << ", crease " << crease;
            }
        }
}

TEST (HudTerrainDesign, FixedControlSelectionSharesHistoryWorkerAndConflictLayer)
{
    TerrainHostFixture fixture;
    LandscapeSource guide;
    guide.entityId = edit::AddPolyline (fixture.doc, { { 10, -1, 10 }, { 20, 1, 10 } }, false);
    Id source = 0;
    std::string error;
    ASSERT_TRUE (AddLandscapeSource (fixture.doc, guide, source, error)) << error;
    const auto vertex = edit::Find (fixture.doc, guide.entityId)->vertices.front ().id;
    eh::Session session (fixture.doc);
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    const auto initialDepth = session.edits.UndoDepth ();
    session.selection.Set (
        { { guide.entityId, edit::Part::Entity, 0 }, { guide.entityId, edit::Part::Vertex, vertex } });
    const auto selection = session.selection.Items ();
    session.selection.Set ({ { guide.entityId, edit::Part::Vertex, vertex }, { fixture.path, edit::Part::Range, 0 } });
    EXPECT_FALSE (eh::SetTerrainControlSelection (session, true));
    EXPECT_TRUE (session.edits.Doc ().landscape->terrainControls.empty ());
    EXPECT_EQ (session.edits.UndoDepth (), initialDepth);
    session.selection.Set ({ { guide.entityId, edit::Part::Vertex, 0 } });
    EXPECT_FALSE (eh::SetTerrainControlSelection (session, true));
    session.selection.Set (selection);
    ASSERT_TRUE (eh::SetTerrainControlSelection (session, true)) << session.message;
    EXPECT_EQ (session.edits.UndoDepth (), initialDepth + 1);
    EXPECT_EQ (session.selection.Items (), selection);
    EXPECT_EQ (session.edits.Doc ().landscape->terrainControls.size (), 2U);
    EXPECT_FALSE (eh::SetTerrainControlSelection (session, true)); // No-op, no history.
    EXPECT_EQ (session.edits.UndoDepth (), initialDepth + 1);
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto* current = eh::CurrentTerrainDesign (session);
    ASSERT_NE (current, nullptr);
    EXPECT_FALSE (current->result.locked.empty ());
    EXPECT_EQ (session.edits.UndoDepth (), initialDepth + 1);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_TRUE (session.edits.Doc ().landscape->terrainControls.empty ());
    ASSERT_TRUE (session.edits.Redo ());
    ASSERT_TRUE (session.edits.Apply ([=] (edit::Document& doc, std::string&) {
        edit::Find (doc, guide.entityId)->vertices.front ().p.z += 1;
        edit::Find (doc, guide.entityId)->vertices.back ().p.z += 1;
        return true;
    }));
    const auto depth = session.edits.UndoDepth ();
    eh::Follow (session, layer);
    EXPECT_EQ (TerrainMesh (layer), nullptr); // Stale old build never draws.
    ASSERT_TRUE (AwaitTerrain (session, layer, true));
    current = eh::CurrentTerrainDesign (session);
    ASSERT_NE (current, nullptr);
    ASSERT_FALSE (current->conflicts.empty ());
    EXPECT_GT (current->conflicts.size (), 1U); // Every displaced split of the fixed line is shown.
    EXPECT_EQ (current->conflicts.front ().code, HybridConflictCode::LockedConflict);
    ASSERT_TRUE (current->conflicts.front ().control);
    EXPECT_EQ (current->conflicts.front ().control->source, source);
    EXPECT_FALSE (layer.points.empty ());
    EXPECT_EQ (session.edits.UndoDepth (), depth); // Worker outcomes never author history.
    ASSERT_TRUE (session.edits.Begin ());
    EXPECT_FALSE (eh::SetTerrainControlSelection (session, false));
    session.edits.Cancel ();
    ASSERT_TRUE (eh::SetTerrainControlSelection (session, false));
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_TRUE (eh::CurrentTerrainDesign (session)->result.locked.empty ());
}

// The user, 2026-10-24: the edited topography was barely visible, and only from below. It lies on
// the host terrain it was captured from, and unstyled it took the layer's Fade there. It is drawn
// lit and held through its host now, and carries contour lines on its own triangles.
TEST (HudTerrainDesign, TheProposedTerrainReadsThroughItsHostAndCarriesContours)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    layer = {};
    session.publishedRevision = UINT64_MAX;
    ASSERT_TRUE (eh::Follow (session, layer));
    const auto* terrain = TerrainMesh (layer);
    ASSERT_NE (terrain, nullptr);
    EXPECT_TRUE (terrain->styled);
    EXPECT_EQ (terrain->style.shading, layers::Shading::Lit);
    EXPECT_GE (terrain->style.occludedOpacity * terrain->style.opacity, 0.6f) << "readable where its host is in front";
    const auto& build = eh::CurrentTerrainDesign (session)->result.build;
    edit::PavedArea surface;
    for (size_t i = 0; i + 2 < build.points.size (); i += 3)
        surface.vertices.push_back ({ build.points[i], build.points[i + 1], build.points[i + 2] });
    surface.triangles = build.triangles;
    size_t lines = 0;
    for (const auto& line : layer.polylines) {
        if (line.behind != layers::Behind::Show || (line.rgba != 0x2F4A35E6u && line.rgba != 0x37523D99u))
            continue;
        ++lines;
        for (size_t k = 0; k + 2 < line.points.size (); k += 3) {
            double z = 0.0;
            ASSERT_TRUE (edit::PavedHeight (surface, { line.points[k], line.points[k + 1], 0 }, z));
            EXPECT_NEAR (z, line.points[k + 2], 1e-6);
        }
    }
    EXPECT_GE (lines, 5u) << "the 12 m the surface falls, about ten levels";
    session.contours = false;
    layer = {};
    session.publishedRevision = UINT64_MAX;
    ASSERT_TRUE (eh::Follow (session, layer));
    for (const auto& line : layer.polylines)
        EXPECT_FALSE (line.behind == layers::Behind::Show && line.rgba == 0x37523D99u) << "hidden with the paths'";
}

TEST (HudTerrainDesign, SimpleTwoTriangleMeshIsRebuiltAroundThePathInsteadOfBridgingItsProfile)
{
    TerrainHostFixture fixture;
    auto mesh = std::make_shared<geomsrv::Mesh> (*fixture.mesh);
    for (size_t i = 2; i < mesh->vertices.size (); i += 3)
        mesh->vertices[i] = 10;
    std::string error;
    Id peak = 0;
    ASSERT_TRUE (edit::InsertPathBreak (fixture.doc, fixture.path, 20, error, &peak)) << error;
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, fixture.path,
        [] (edit::Path& path, std::string&) {
            for (auto& point : path.profile)
                point.z = point.station == 20 ? 13 : 11;
            for (auto& section : path.sections) {
                section.widthLeft = section.widthRight = 8;
                section.crossfall = 0;
            }
            return true;
        },
        error))
        << error;
    eh::Session session (fixture.doc);
    session.terrainBand = true; // the band's own lines and provenance
    ASSERT_TRUE (eh::StartTerrainDesign (session, mesh, fixture.path)) << session.message;
    const auto depth = session.edits.UndoDepth ();
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto* current = eh::CurrentTerrainDesign (session);
    ASSERT_NE (current, nullptr);
    const auto& result = current->result;
    EXPECT_GT (result.build.points.size (), mesh->vertices.size ());
    EXPECT_GT (result.build.triangles.size (), mesh->triangles.size ());
    EXPECT_EQ (result.build.inputPointCount, 4U);
    EXPECT_EQ (result.meshProvenance.counts.vertices, result.build.points.size () / 3);
    EXPECT_EQ (result.meshProvenance.counts.faces, result.build.triangles.size () / 3);
    EXPECT_GT (std::count_if (result.build.lines.begin (), result.build.lines.end (),
                              [] (const auto& line) { return line.kind == HybridLineKind::Section; }),
               1);
    const std::array<double, 6> queries { 20, -7, 20, 0, 20, 7 };
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    ASSERT_TRUE (SampleTerrain ({ result.build.points, result.build.triangles }, queries, .25, samples, summary, error))
        << error;
    for (const auto& sample : samples) {
        EXPECT_TRUE (sample.onSurface);
        EXPECT_NEAR (sample.height, 13, 1e-9);
    }
    EXPECT_NE (TerrainMesh (layer), nullptr);
    EXPECT_EQ (session.edits.UndoDepth (), depth); // Derived detail is never authored geometry/history.
    EXPECT_EQ (mesh->vertices.size (), 12U);
    EXPECT_EQ (mesh->triangles.size (), 6U);
}

TEST (HudTerrainDesign, TheOutlineGradesByDefaultAndTheBandIsOneSwitchAway)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto lines = [&] { return eh::CurrentTerrainDesign (session)->result.build.lines.size (); };
    EXPECT_EQ (lines (), 0U) << "outline grading has no band lines";
    const auto depth = session.edits.UndoDepth ();
    eh::SetTerrainBand (session, true);
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline && (!eh::CurrentTerrainDesign (session) || lines () == 0)) {
        eh::Follow (session, layer);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    ASSERT_NE (eh::CurrentTerrainDesign (session), nullptr);
    EXPECT_GT (lines (), 0U) << "the band's edges, ends and sections";
    EXPECT_EQ (session.edits.UndoDepth (), depth) << "switching authors nothing";
}

TEST (HudTerrainDesign, OneHistoryOwnsTheDesignAndItsDerivedWorkerLayer)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.selection.Set ({ { fixture.path, edit::Part::Entity, 0 } });
    const auto selection = session.selection.Items ();
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    EXPECT_EQ (session.selection.Items (), selection);
    const auto& objects = session.edits.Doc ().landscape->objects;
    ASSERT_EQ (objects.size (), 1U);
    EXPECT_EQ (objects[0].kind, ObjectKind::Path);
    EXPECT_EQ (objects[0].pathId, fixture.path);
    const auto authored = session.edits.Doc ();
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_TRUE (layers::Validate (layer).empty ()) << layers::Validate (layer);
    EXPECT_EQ (session.edits.Doc ().landscape, authored.landscape);
    EXPECT_EQ (session.edits.Doc ().revision, authored.revision);
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    const auto baseline = TerrainMesh (layer)->points;
    // Every field of the derived state stays on the same shared session when
    // another host takes it; rebinding the same path creates no authored edit.
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (TerrainMesh (layer)->points, baseline);
    ASSERT_TRUE (session.edits.Undo ());
    eh::Follow (session, layer);
    EXPECT_EQ (TerrainMesh (layer), nullptr);
    ASSERT_TRUE (session.edits.Redo ());
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (TerrainMesh (layer)->points, baseline);
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
}

TEST (HudTerrainDesign, ANewMeshGenerationRetracesContoursEvenWhenTheTriangleCountIsUnchanged)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.contourStep = 0.5;
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path));
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto contours = [] (const layers::Layer& value) {
        std::vector<std::vector<double>> lines;
        for (const auto& line : value.polylines)
            if (line.rgba == 0x2F4A35E6u || line.rgba == 0x37523D99u)
                lines.push_back (line.points);
        return lines;
    };
    const auto before = contours (layer);
    ASSERT_FALSE (before.empty ());
    const auto* previous = eh::CurrentTerrainDesign (session);
    const auto triangles = previous->result.build.triangles.size ();
    const auto generation = previous->generation;
    ASSERT_TRUE (session.edits.Apply ([&] (edit::Document& doc, std::string&) {
        for (auto& grade : edit::FindPath (doc, fixture.path)->profile)
            grade.z += 0.02;
        return true;
    }));
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto* current = eh::CurrentTerrainDesign (session);
    ASSERT_EQ (current, previous) << "publisher reuses its optional storage";
    ASSERT_EQ (current->result.build.triangles.size (), triangles);
    ASSERT_GT (current->generation, generation);
    edit::PavedArea surface;
    for (size_t i = 0; i + 2 < current->result.build.points.size (); i += 3)
        surface.vertices.push_back ({ current->result.build.points[i], current->result.build.points[i + 1],
                                      current->result.build.points[i + 2] });
    surface.triangles = current->result.build.triangles;
    std::vector<std::vector<double>> expected;
    for (const auto& contour : edit::SurfaceContours (surface, session.contourStep)) {
        std::vector<double> line;
        for (const auto p : contour.points)
            line.insert (line.end (), { p.x, p.y, p.z });
        expected.push_back (std::move (line));
    }
    EXPECT_TRUE (contours (layer) == expected) << "contours must belong to the current mesh generation";
    EXPECT_FALSE (contours (layer) == before) << "a small edit cannot keep the previous mesh's contours";
}

TEST (HudTerrainDesign, TypedCrossSectionApplyRebuildsTheExistingPreviewWithoutAddingWorkerHistory)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (session.terrainPath, fixture.path);
    const auto original = eh::CurrentTerrainDesign (session)->result.build.points;
    const auto section = edit::FindPath (session.edits.Doc (), fixture.path)->sections.front ().id;
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, section)) << session.message;
    session.terrainSectionDraft->value.widthLeft = 2;
    session.terrainSectionDraft->value.widthRight = 2;
    session.terrainSectionDraft->value.crossfall = 1;
    session.terrainSectionDraft->value.fall = edit::Fall::Crown;
    session.terrainSectionDraft->value.cutSlope = 60;
    const auto depth = session.edits.UndoDepth ();
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    EXPECT_EQ (session.edits.UndoDepth (), depth + 1);
    eh::Follow (session, layer);
    EXPECT_EQ (TerrainMesh (layer), nullptr);
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_NE (eh::CurrentTerrainDesign (session)->result.build.points, original);
    EXPECT_GT (eh::CurrentTerrainDesign (session)->result.build.site.cut, 0);
    EXPECT_EQ (session.edits.UndoDepth (), depth + 1);
    ASSERT_TRUE (session.edits.Undo ());
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (eh::CurrentTerrainDesign (session)->result.build.points, original);
}

TEST (HudTerrainDesign, EditsCoalesceUntilReleaseAndStaleWorkersNeverDraw)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path));
    layers::Layer layer;
    eh::Follow (session, layer); // launch the original revision
    ASSERT_TRUE (session.edits.Begin ());
    const auto raise = [&] (edit::Document& doc, std::string& error) {
        return edit::EditPath (
            doc, fixture.path,
            [] (edit::Path& path, std::string&) {
                for (auto& point : path.profile)
                    point.z += 0.1;
                return true;
            },
            error);
    };
    ASSERT_TRUE (session.edits.Move (raise));
    const auto depth = session.edits.UndoDepth ();
    for (int i = 0; i < 20; ++i) {
        eh::Follow (session, layer);
        EXPECT_EQ (TerrainMesh (layer), nullptr);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    EXPECT_EQ (session.edits.UndoDepth (), depth);
    ASSERT_TRUE (session.edits.End ());
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (session.edits.UndoDepth (), depth + 1);
    ASSERT_NE (eh::CurrentTerrainDesign (session), nullptr);
    EXPECT_TRUE (eh::CurrentTerrainDesign (session)->result.conflicts.empty ());
}

TEST (HudTerrainDesign, RefusedDesignDrawsLocatedPlacesAndStartRefusalsAreAtomic)
{
    geomsrv::archviz::test::LogCapture log;
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    auto bad = std::make_shared<geomsrv::Mesh> (*fixture.mesh);
    bad->triangles[0] = UINT32_MAX;
    const auto before = session.edits.Doc ().landscape;
    EXPECT_FALSE (eh::StartTerrainDesign (session, bad, fixture.path));
    EXPECT_EQ (session.edits.Doc ().landscape, before);
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
    EXPECT_FALSE (session.terrainDesign);
    std::string error;
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, fixture.path,
        [] (edit::Path& path, std::string&) {
            for (auto& section : path.sections)
                section.cutSlope = section.fillSlope = 0.1;
            return true;
        },
        error));
    eh::Session refused (fixture.doc);
    refused.terrainBand = true; // located refusals are the band's
    ASSERT_TRUE (eh::StartTerrainDesign (refused, fixture.mesh, fixture.path));
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (refused, layer, true));
    EXPECT_EQ (TerrainMesh (layer), nullptr);
    EXPECT_TRUE (layers::Validate (layer).empty ()) << layers::Validate (layer);
    EXPECT_EQ (refused.edits.UndoDepth (), 1U);
    const auto status = eh::GetTerrainDesignStatus (refused);
    ASSERT_FALSE (status.error.empty ());
    EXPECT_TRUE (std::any_of (log.lines.begin (), log.lines.end (), [&] (const auto& line) {
        return line.find ("TERRAIN worker completed") != std::string::npos &&
               line.find ("built=0") != std::string::npos && line.find (status.error) != std::string::npos;
    }));
    EXPECT_TRUE (std::any_of (log.lines.begin (), log.lines.end (), [] (const auto& line) {
        return line.find ("TERRAIN layer no-proposal") != std::string::npos;
    }));
    EXPECT_FALSE (std::any_of (log.lines.begin (), log.lines.end (), [] (const auto& line) {
        return line.find ("TERRAIN layer proposed") != std::string::npos;
    }));
}

TEST (HudTerrainDesign, NativeSurfaceBuildsOnWorkerAndNativeChangeHidesItWithoutHistory)
{
    geomsrv::archviz::test::LogCapture log;
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    auto valid = std::make_shared<bool> (true);
    eh::TerrainDesignBaseline baseline { fixture.mesh, true, [valid] { return *valid; } };
    ASSERT_TRUE (eh::StartTerrainDesign (session, baseline, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto* outcome = eh::CurrentTerrainDesign (session);
    ASSERT_NE (outcome, nullptr);
    ASSERT_NE (outcome->capture, nullptr);
    EXPECT_NE (outcome->capture->native, nullptr);
    EXPECT_EQ (outcome->capture->provenance.table.context.tableId, 5U);
    EXPECT_TRUE (std::any_of (log.lines.begin (), log.lines.end (), [] (const auto& line) {
        return line.find ("TERRAIN worker completed") != std::string::npos &&
               line.find ("surfacePrepared=1") != std::string::npos;
    }));
    const auto depth = session.edits.UndoDepth ();
    const auto revision = session.edits.Doc ().revision;
    *valid = false;
    EXPECT_EQ (eh::CurrentTerrainDesign (session), nullptr);
    eh::Follow (session, layer);
    EXPECT_EQ (TerrainMesh (layer), nullptr);
    const auto status = eh::GetTerrainDesignStatus (session);
    EXPECT_FALSE (status.current);
    EXPECT_EQ (status.error.find ("TerrainCaptureStale"), 0U);
    EXPECT_EQ (session.edits.Doc ().revision, revision);
    EXPECT_EQ (session.edits.UndoDepth (), depth);
    const auto logCount = log.lines.size ();
    eh::Follow (session, layer);
    EXPECT_EQ (log.lines.size (), logCount); // One invalidation transition, not a log per frame.
    EXPECT_TRUE (std::any_of (log.lines.begin (), log.lines.end (), [] (const auto& line) {
        return line.find ("TERRAIN baseline invalidated: TerrainCaptureStale") != std::string::npos;
    }));
    EXPECT_FALSE (eh::StartTerrainDesign (session, baseline, fixture.path));
    *valid = true;
    ASSERT_TRUE (eh::StartTerrainDesign (session, baseline, fixture.path));
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (session.edits.UndoDepth (), depth);
}

TEST (HudTerrainDesign, NativeSurfacePreparationRefusalIsLoggedWithoutDrawingTheRawSolid)
{
    geomsrv::archviz::test::LogCapture log;
    TerrainHostFixture fixture;
    auto mesh = std::make_shared<geomsrv::Mesh> (*fixture.mesh);
    for (size_t i = 0; i < mesh->triangles.size (); i += 3)
        std::swap (mesh->triangles[i + 1], mesh->triangles[i + 2]); // No upward surface.
    eh::Session session (fixture.doc);
    ASSERT_TRUE (eh::StartTerrainDesign (session, eh::TerrainDesignBaseline { mesh, true, {} }, fixture.path));
    const auto depth = session.edits.UndoDepth ();
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer, true));
    EXPECT_EQ (TerrainMesh (layer), nullptr);
    const auto* outcome = eh::CurrentTerrainDesign (session);
    ASSERT_NE (outcome, nullptr);
    ASSERT_NE (outcome->capture, nullptr);
    EXPECT_FALSE (outcome->capture->native); // Retained raw capture is not proof of a drawable surface.
    EXPECT_FALSE (outcome->error.empty ());
    EXPECT_EQ (session.edits.UndoDepth (), depth);
    EXPECT_TRUE (std::any_of (log.lines.begin (), log.lines.end (), [&] (const auto& line) {
        return line.find ("TERRAIN worker completed") != std::string::npos &&
               line.find ("built=0 surfacePrepared=0") != std::string::npos &&
               line.find (outcome->error) != std::string::npos;
    }));
}
