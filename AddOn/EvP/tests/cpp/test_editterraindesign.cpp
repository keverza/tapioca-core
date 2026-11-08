#include "Edit/EditTerrainDesign.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "virtual_editor_debug_replay.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "Edit/EditTerrainOutline.hpp"
#include "Geometry/GeometryRustOutlineTerrain.hpp"
#include "Geometry/Mesh.hpp"
#include "Geometry/ClipperPlanRing.hpp"
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
#include <map>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

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
                    section.crossfall = edit::PercentCrossfall (0);
                    section.cutSlope = { TerrainSlope::Unit::Percent, 50, 0 }; // 1:2
                    section.fillSlope = { TerrainSlope::Unit::Percent, 50, 0 };
                    section.rounding = 0; // the worked volumes are the sharp ones
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

TEST (EditTerrainDesign, EquivalentPathCutFormsKeepAnalyticDaylightQuantitiesAndBothAdapters)
{
    DesignFixture fixture;
    const auto baseline = fixture.points;
    double bandCut = -1, bandFill = -1, outlineCut = -1, outlineFill = -1;
    for (const TerrainSlope cut : { TerrainSlope { TerrainSlope::Unit::Percent, 50, 0 },
                                    { TerrainSlope::Unit::Angle, std::atan (0.5), 0 },
                                    { TerrainSlope::Unit::RiseRun, 1.5, 3 },
                                    { TerrainSlope::Unit::Height, 2.5, 5 } }) {
        std::string error;
        ASSERT_TRUE (edit::EditPath (
            fixture.doc, fixture.path,
            [cut] (auto& path, auto&) {
                path.sections[0].cutSlope = cut;
                return true;
            },
            error))
            << error;
        ++fixture.doc.revision;
        fixture.Capture ();
        TerrainDesignResult band;
        ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), band, error)) << error;
        for (const auto& sides : band.build.sides)
            for (const auto& side : sides) {
                EXPECT_EQ (side.roots, 1u);
                EXPECT_NEAR (side.first, 4.0 / 3.0, 1e-9);
            }
        if (bandCut < 0) {
            bandCut = band.build.site.cut;
            bandFill = band.build.site.fill;
        }
        EXPECT_NEAR (band.build.site.cut, bandCut, 1e-7);
        EXPECT_NEAR (band.build.site.fill, bandFill, 1e-7);
        EXPECT_GE (band.build.site.cut, 80.0 / 3.0 - 1e-9);
        EXPECT_GE (band.build.site.fill, 80.0 / 3.0 - 1e-9);
        const auto options = OutlineOptionsFor (fixture.doc, fixture.path, 10);
        EXPECT_EQ (options.cut, cut);
        EXPECT_EQ (options.fill, (TerrainSlope { TerrainSlope::Unit::Percent, 50, 0 }));
        TerrainDesignResult outline;
        uint32_t unreached = 0;
        ASSERT_TRUE (EvaluateOutlineDesign (fixture.doc, fixture.Input (), options, outline, unreached, error))
            << error;
        if (outlineCut < 0) {
            outlineCut = outline.build.site.cut;
            outlineFill = outline.build.site.fill;
        }
        EXPECT_GT (outline.build.site.cut, 0);
        EXPECT_GT (outline.build.site.fill, 0);
        EXPECT_NEAR (outline.build.site.cut, outlineCut, 1e-6);
        EXPECT_NEAR (outline.build.site.fill, outlineFill, 1e-6);
        const std::array<double, 6> queries { 5, 0, 20, 0, -15, -20 };
        for (const auto* built : { &band.build, &outline.build }) {
            std::vector<TerrainSample> samples;
            TerrainFieldSummary summary;
            ASSERT_TRUE (SampleTerrain ({ built->points, built->triangles }, queries, 0.25, samples, summary, error))
                << error;
            ASSERT_EQ (samples.size (), 3u);
            EXPECT_NEAR (samples[0].height, 10.25, 1e-7);
            EXPECT_NEAR (samples[1].height, 11, 1e-7);
            EXPECT_NEAR (samples[2].height, 13.25, 1e-7);
        }
        EXPECT_EQ (fixture.doc.paths[0].sections[0].cutSlope, cut);
        EXPECT_EQ (fixture.doc.paths[0].sections[0].fillSlope, (TerrainSlope { TerrainSlope::Unit::Percent, 50, 0 }));
        EXPECT_EQ (fixture.points, baseline);
    }
}

TEST (EditTerrainDesign, EquivalentPathFillFormsKeepTypedCutAnalyticDaylightQuantitiesAndBothAdapters)
{
    DesignFixture fixture;
    const auto baseline = fixture.points;
    const TerrainSlope cut { TerrainSlope::Unit::Height, 2.5, 5 };
    double bandCut = -1, bandFill = -1, outlineCut = -1, outlineFill = -1;
    for (const TerrainSlope fill : { TerrainSlope { TerrainSlope::Unit::Percent, 50, 0 },
                                     { TerrainSlope::Unit::Angle, std::atan (0.5), 0 },
                                     { TerrainSlope::Unit::RiseRun, 1.5, 3 },
                                     { TerrainSlope::Unit::Height, 2.5, 5 } }) {
        std::string error;
        ASSERT_TRUE (edit::EditPath (
            fixture.doc, fixture.path,
            [cut, fill] (auto& path, auto&) {
                path.sections[0].cutSlope = cut;
                path.sections[0].fillSlope = fill;
                return true;
            },
            error))
            << error;
        ++fixture.doc.revision;
        fixture.Capture ();
        const auto provenance = fixture.provenance;
        TerrainDesignResult band;
        ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), band, error)) << error;
        for (const auto& sides : band.build.sides)
            for (const auto& side : sides) {
                EXPECT_EQ (side.roots, 1u);
                EXPECT_NEAR (side.first, 4.0 / 3.0, 1e-9);
            }
        if (bandCut < 0) {
            bandCut = band.build.site.cut;
            bandFill = band.build.site.fill;
        }
        EXPECT_NEAR (band.build.site.cut, bandCut, 1e-7);
        EXPECT_NEAR (band.build.site.fill, bandFill, 1e-7);
        EXPECT_GE (band.build.site.cut, 80.0 / 3.0 - 1e-9);
        EXPECT_GE (band.build.site.fill, 80.0 / 3.0 - 1e-9);
        const auto options = OutlineOptionsFor (fixture.doc, fixture.path, 10);
        EXPECT_EQ (options.cut, cut);
        EXPECT_EQ (options.fill, fill);
        TerrainDesignResult outline;
        uint32_t unreached = 0;
        ASSERT_TRUE (EvaluateOutlineDesign (fixture.doc, fixture.Input (), options, outline, unreached, error))
            << error;
        if (outlineCut < 0) {
            outlineCut = outline.build.site.cut;
            outlineFill = outline.build.site.fill;
        }
        EXPECT_GT (outline.build.site.cut, 0);
        EXPECT_GT (outline.build.site.fill, 0);
        EXPECT_NEAR (outline.build.site.cut, outlineCut, 1e-6);
        EXPECT_NEAR (outline.build.site.fill, outlineFill, 1e-6);
        const std::array<double, 6> queries { 5, 0, 20, 0, -15, -20 };
        for (const auto* built : { &band.build, &outline.build }) {
            std::vector<TerrainSample> samples;
            TerrainFieldSummary summary;
            ASSERT_TRUE (SampleTerrain ({ built->points, built->triangles }, queries, 0.25, samples, summary, error))
                << error;
            ASSERT_EQ (samples.size (), 3u);
            EXPECT_NEAR (samples[0].height, 10.25, 1e-7);
            EXPECT_NEAR (samples[1].height, 11, 1e-7);
            EXPECT_NEAR (samples[2].height, 13.25, 1e-7);
        }
        EXPECT_EQ (fixture.doc.paths[0].sections[0].cutSlope, cut);
        EXPECT_EQ (fixture.doc.paths[0].sections[0].fillSlope, fill);
        EXPECT_EQ (fixture.points, baseline);
        EXPECT_EQ (fixture.provenance, provenance);
    }
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
                section.crossfall = edit::PercentCrossfall (2);
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

TEST (EditTerrainDesign, CrossfallContractKeepsSignedOutlineAndExistingNegativeBandRefusal)
{
    for (const auto fall : { edit::Fall::Left, edit::Fall::Right, edit::Fall::Crown })
        for (double crossfall : { 2.0, -2.0, 0.0 }) {
            SCOPED_TRACE ("fall=" + std::to_string (static_cast<int> (fall)) +
                          " crossfall=" + std::to_string (crossfall));
            DesignFixture fixture;
            std::string error;
            ASSERT_TRUE (edit::SetPathCrossfall (fixture.doc, fixture.path, fixture.doc.paths[0].sections[0].id,
                                                 crossfall, fall, error))
                << error;
            ++fixture.doc.revision;
            fixture.Capture ();
            const auto baseline = fixture.points;
            const auto provenance = fixture.provenance;
            TerrainDesignResult band;
            band.region = { 123, 456, 789 };
            const auto original = band;
            if (crossfall < 0) {
                EXPECT_FALSE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), band, error));
                EXPECT_NE (error.find ("DesignSlope"), std::string::npos) << error;
                EXPECT_EQ (band, original) << "negative-band capability is not added by the crossfall unit slice";
            }
            else
                ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), fixture.Options (), band, error))
                    << error;
            TerrainDesignResult outline;
            uint32_t unreached = 0;
            const auto options = OutlineOptionsFor (fixture.doc, fixture.path, 10);
            ASSERT_TRUE (EvaluateOutlineDesign (fixture.doc, fixture.Input (), options, outline, unreached, error))
                << error;
            // At x=20 the profile is 11 m. One metre left/right drops 2 cm on
            // the specified side; negative raw Crown is a valley, not a crown.
            const double left = (fall == edit::Fall::Right ? 1 : -1) * crossfall / 100;
            const double right = (fall == edit::Fall::Left ? 1 : -1) * crossfall / 100;
            const auto paved = edit::Pave (fixture.doc.paths);
            double pavedLeft = 0, pavedRight = 0;
            ASSERT_TRUE (edit::PavedHeight (paved, { 20, 1, 0 }, pavedLeft));
            ASSERT_TRUE (edit::PavedHeight (paved, { 20, -1, 0 }, pavedRight));
            EXPECT_NEAR (pavedLeft, 11 + left, 1e-9);
            EXPECT_NEAR (pavedRight, 11 + right, 1e-9);
            const std::array<double, 6> queries { 20, 1, 20, -1, -15, -20 };
            for (const auto* built : { crossfall < 0 ? &outline.build : &band.build, &outline.build }) {
                std::vector<TerrainSample> samples;
                TerrainFieldSummary summary;
                ASSERT_TRUE (
                    SampleTerrain ({ built->points, built->triangles }, queries, 0.25, samples, summary, error))
                    << error;
                ASSERT_EQ (samples.size (), 3u);
                EXPECT_NEAR (samples[0].height, 11 + left, 1e-7);
                EXPECT_NEAR (samples[1].height, 11 + right, 1e-7);
                EXPECT_NEAR (samples[2].height, 13.25, 1e-7);
            }
            EXPECT_EQ (fixture.points, baseline);
            EXPECT_EQ (fixture.provenance, provenance);
            EXPECT_EQ (fixture.doc.paths[0].sections[0].crossfall, edit::PercentCrossfall (crossfall));
            EXPECT_EQ (fixture.doc.paths[0].sections[0].fall, fall);
        }
}

TEST (EditTerrainDesign, TypedCrossfallProjectsIntoBothAdaptersWithoutEnablingNegativeBands)
{
    using Unit = TerrainSlope::Unit;
    for (const edit::PathCrossfall crossfall : { edit::PathCrossfall { { Unit::Angle, std::atan (.02), 0 }, false },
                                                 { { Unit::RiseRun, 1, 50 }, false },
                                                 { { Unit::Height, 1, 50 }, false },
                                                 { { Unit::RiseRun, 1, 50 }, true } }) {
        DesignFixture fixture;
        std::string error;
        ASSERT_TRUE (edit::SetPathCrossfall (fixture.doc, fixture.path, fixture.doc.paths[0].sections[0].id, crossfall,
                                             edit::Fall::Crown, error))
            << error;
        ++fixture.doc.revision;
        fixture.Capture ();
        const double percent = crossfall.negative ? -2 : 2;
        const auto options = fixture.Options ();
        if (crossfall.negative) {
            TerrainDesignResult refused;
            EXPECT_FALSE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, refused, error));
            EXPECT_NE (error.find ("DesignSlope"), std::string::npos) << error;
        }
        else {
            TerrainDesignResult band;
            ASSERT_TRUE (EvaluateTerrainDesign (fixture.doc, fixture.Input (), options, band, error)) << error;
            std::vector<TerrainSample> samples;
            TerrainFieldSummary summary;
            ASSERT_TRUE (SampleTerrain ({ band.build.points, band.build.triangles },
                                        std::array<double, 4> { 20, 1, 20, -1 }, .25, samples, summary, error))
                << error;
            ASSERT_EQ (samples.size (), 2u);
            EXPECT_NEAR (samples[0].height, 10.98, 1e-7);
            EXPECT_NEAR (samples[1].height, 10.98, 1e-7);
        }
        TerrainDesignResult outline;
        uint32_t unreached = 0;
        ASSERT_TRUE (EvaluateOutlineDesign (fixture.doc, fixture.Input (),
                                            OutlineOptionsFor (fixture.doc, fixture.path, 10), outline, unreached,
                                            error))
            << error;
        std::vector<TerrainSample> samples;
        TerrainFieldSummary summary;
        ASSERT_TRUE (SampleTerrain ({ outline.build.points, outline.build.triangles },
                                    std::array<double, 4> { 20, 1, 20, -1 }, .25, samples, summary, error))
            << error;
        ASSERT_EQ (samples.size (), 2u);
        EXPECT_NEAR (samples[0].height, 11 - percent * .01, 1e-7);
        EXPECT_NEAR (samples[1].height, 11 - percent * .01, 1e-7);
    }
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
    // #150: the toe rounds across the daylight as a fillet, never steeper than the face, so
    // it adds earthwork where the face meets the ground: a fill's toe rides above both and a
    // cut's below both. Measured on this fixture: fill 27.94 -> 35.13 m3, cut 27.78 -> 35.05 m3.
    EXPECT_GT (rounded.build.site.fill, sharp.build.site.fill);
    EXPECT_GT (rounded.build.site.cut, sharp.build.site.cut);
    EXPECT_LT (rounded.build.site.fill, 1.4 * sharp.build.site.fill);
    EXPECT_LT (rounded.build.site.cut, 1.4 * sharp.build.site.cut);
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

// #176: the outline-geometry cache returns the last build's geometry while the document's
// authored content, the terrain and the options are the same, and builds again when any of
// them changes; what it returns is what a fresh build makes.
TEST (EditTerrainDesignJob, TheOutlineGeometryCacheReusesOnlyUnchangedInputs176)
{
    DesignFixture fixture;
    std::string error;
    const TerrainMeshView mesh { fixture.points, fixture.triangles };
    const auto options = OutlineOptionsFor (fixture.doc, fixture.path, 10.0);
    OutlineGeometryCache cache;
    OutlineGeometry first, again, fresh;
    ASSERT_TRUE (cache.Build (fixture.doc, mesh, options, first, error)) << error;
    ASSERT_TRUE (BuildOutlineGeometry (fixture.doc, mesh, options, fresh, error)) << error;
    EXPECT_EQ (first.surfacePoints, fresh.surfacePoints);
    EXPECT_EQ (first.lines, fresh.lines);
    EXPECT_EQ (first.region, fresh.region);
    // The same inputs, another revision number: reused.
    auto bumped = fixture.doc;
    ++bumped.revision;
    ASSERT_TRUE (cache.Build (bumped, mesh, options, again, error)) << error;
    EXPECT_EQ (cache.Hits (), 1U);
    EXPECT_EQ (cache.Builds (), 1U);
    EXPECT_EQ (again.surfacePoints, first.surfacePoints);
    // A path moved: built again, as a fresh build makes it.
    auto moved = fixture.doc;
    ASSERT_TRUE (edit::EditPath (
        moved, fixture.path,
        [] (edit::Path& path, std::string&) {
            for (auto& point : path.points)
                point.p.y += 0.5;
            return true;
        },
        error))
        << error;
    OutlineGeometry shifted, shiftedFresh;
    ASSERT_TRUE (cache.Build (moved, mesh, options, shifted, error)) << error;
    ASSERT_TRUE (BuildOutlineGeometry (moved, mesh, options, shiftedFresh, error)) << error;
    EXPECT_EQ (cache.Builds (), 2U);
    EXPECT_EQ (shifted.surfacePoints, shiftedFresh.surfacePoints);
    EXPECT_NE (shifted.surfacePoints, first.surfacePoints);
    // Other options, another terrain: built again.
    auto coarser = options;
    coarser.pavedTolerance = 0.01;
    ASSERT_TRUE (cache.Build (moved, mesh, coarser, shifted, error)) << error;
    EXPECT_EQ (cache.Builds (), 3U);
    auto raised = fixture.points;
    for (size_t i = 2; i < raised.size (); i += 3)
        raised[i] += 0.1;
    ASSERT_TRUE (cache.Build (moved, { raised, fixture.triangles }, coarser, shifted, error)) << error;
    EXPECT_EQ (cache.Builds (), 4U);
    EXPECT_EQ (cache.Hits (), 1U);
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
    // Only the display may still show it, and only within its session and terrain.
    EXPECT_EQ (publisher.LastPublished (edited), publisher.Current (fixture.doc));
    auto foreign = edited;
    foreign.landscape->sessionUuid[0] ^= 1;
    EXPECT_EQ (publisher.LastPublished (foreign), nullptr);
    foreign = edited;
    foreign.landscape->sourceTerrainGuid += "-other";
    EXPECT_EQ (publisher.LastPublished (foreign), nullptr);
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

// #112 P4b / R8: the publisher splits latest status/diagnostics from the last
// good publication. A failed or stale rebuild reports its diagnostic but never
// replaces the drawable/quantity snapshot.
TEST (EditTerrainDesignJob, ARefusedRebuildKeepsTheLastGoodPublicationAndReportsItsDiagnostic)
{
    DesignFixture fixture;
    TerrainDesignPublisher publisher;
    std::string error;
    TerrainDesignJob good;
    auto options = fixture.Options ();
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, {}, good, error)) << error;
    auto built = RunTerrainDesign (good);
    ASSERT_TRUE (built.built) << built.error;
    const auto goodResult = built.result;
    ASSERT_TRUE (publisher.Publish (fixture.doc, std::move (built), error)) << error;
    ASSERT_NE (publisher.Current (fixture.doc), nullptr);

    // An edit moves the path far off the terrain: the rebuild refuses.
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, fixture.path,
        [] (edit::Path& path, std::string&) {
            for (auto& point : path.points)
                point.p.x += 500;
            return true;
        },
        error))
        << error;
    ++fixture.doc.revision; // The session publishes an accepted edit as a new revision.
    fixture.Capture ();     // Restamp the capture/provenance for the new revision.
    TerrainDesignJob refused;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, {}, refused, error)) << error;
    auto failure = RunTerrainDesign (refused);
    ASSERT_FALSE (failure.built);
    ASSERT_FALSE (failure.error.empty ());
    const auto failureError = failure.error;
    // The refusal is accepted as the latest status/diagnostic...
    ASSERT_TRUE (publisher.Publish (fixture.doc, std::move (failure), error)) << error;
    const auto* latest = publisher.LatestOutcome (fixture.doc);
    ASSERT_NE (latest, nullptr);
    EXPECT_FALSE (latest->built);
    EXPECT_EQ (latest->error, failureError);
    // ...while the last good publication stays the drawable/quantity snapshot.
    EXPECT_EQ (publisher.Current (fixture.doc), nullptr) << "It describes the old revision.";
    const auto* last = publisher.LastPublished (fixture.doc);
    ASSERT_NE (last, nullptr);
    EXPECT_TRUE (last->built);
    EXPECT_EQ (last->result, goodResult);
    // A complete rebuild of the current document replaces the snapshot again.
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, fixture.path,
        [] (edit::Path& path, std::string&) {
            for (auto& point : path.points)
                point.p.x -= 500;
            return true;
        },
        error))
        << error;
    ++fixture.doc.revision;
    fixture.Capture ();
    TerrainDesignJob rebuilt;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, {}, rebuilt, error)) << error;
    auto again = RunTerrainDesign (rebuilt);
    ASSERT_TRUE (again.built) << again.error;
    ASSERT_TRUE (publisher.Publish (fixture.doc, std::move (again), error)) << error;
    ASSERT_NE (publisher.Current (fixture.doc), nullptr);
    EXPECT_NE (publisher.Current (fixture.doc)->result, goodResult) << "The new build replaces the snapshot.";
    EXPECT_EQ (publisher.Latest (), 3U);
}

// #112 P4b: a unified-lane outcome carries the neutral published snapshot; the
// publisher validates its identity (revision/session/source terrain/plan hash)
// before accepting it.
TEST (EditTerrainDesignJob, APublishedSnapshotWithAMismatchedIdentityIsRefused)
{
    DesignFixture fixture;
    TerrainDesignPublisher publisher;
    std::string error;
    TerrainDesignJob job;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), fixture.Options (), {}, job, error)) << error;
    auto outcome = RunTerrainDesign (job);
    ASSERT_TRUE (outcome.built) << outcome.error;
    auto published = std::make_shared<PublishedTerrain> ();
    published->revision = outcome.revision + 1;
    published->session = outcome.session;
    published->sourceTerrainGuid = outcome.sourceTerrainGuid;
    published->planHash = 1;
    outcome.published = std::move (published);
    EXPECT_FALSE (publisher.Publish (fixture.doc, std::move (outcome), error));
    EXPECT_EQ (error.rfind ("ProvenanceStale", 0), 0U) << error;
    EXPECT_EQ (publisher.LatestOutcome (fixture.doc), nullptr);
    EXPECT_EQ (publisher.Current (fixture.doc), nullptr);
    EXPECT_EQ (publisher.LastPublished (fixture.doc), nullptr);
}

namespace {
// Samples both build meshes on a regular grid over the ring's bounds and
// returns the max absolute height difference where both surfaces answer.
double MaxSampledHeightError (const std::vector<double>& a, const std::vector<uint32_t>& trianglesA,
                              const std::vector<double>& b, const std::vector<uint32_t>& trianglesB,
                              std::span<const double> ring, std::string& error)
{
    std::array<double, 4> box { 1e300, 1e300, -1e300, -1e300 };
    for (size_t at = 0; at + 2 < ring.size (); at += 3) {
        box[0] = std::min (box[0], ring[at]);
        box[1] = std::min (box[1], ring[at + 1]);
        box[2] = std::max (box[2], ring[at]);
        box[3] = std::max (box[3], ring[at + 1]);
    }
    std::vector<double> queries;
    for (double y = box[1] + 0.125; y < box[3]; y += 0.25)
        for (double x = box[0] + 0.125; x < box[2]; x += 0.25)
            queries.insert (queries.end (), { x, y });
    std::vector<TerrainSample> samplesA, samplesB;
    TerrainFieldSummary summaryA, summaryB;
    if (!SampleTerrain ({ a, trianglesA }, queries, 0.25, samplesA, summaryA, error) ||
        !SampleTerrain ({ b, trianglesB }, queries, 0.25, samplesB, summaryB, error))
        return -1;
    double maxError = 0;
    for (size_t index = 0; index < samplesA.size (); ++index)
        if (samplesA[index].onSurface && samplesB[index].onSurface)
            maxError = std::max (maxError, std::fabs (samplesA[index].height - samplesB[index].height));
    return maxError;
}
} // namespace

// #112 P4c: a band path-only scene (no region objects) routes through the
// unified authority and publishes, mirroring the legacy band outcome field for
// field; the legacy lane stays the parity oracle.
TEST (EditTerrainDesignJob, BandPathOnlyBuildsThroughTheUnifiedLaneWithLegacyParity)
{
    DesignFixture fixture;
    TerrainDesignPublisher publisher;
    std::string error;
    TerrainDesignJob job;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), fixture.Options (), {}, job, error)) << error;
    ASSERT_TRUE (TerrainRegionObjects (fixture.doc).empty ());
    const auto unified = RunLandscapeTerrainDesign (job);
    ASSERT_TRUE (unified.built) << unified.error;
    ASSERT_TRUE (unified.published) << "band path-only publishes the unified snapshot";
    EXPECT_FALSE (unified.regions) << "path-only never ran the region stage";
    const auto legacy = RunTerrainDesign (job);
    ASSERT_TRUE (legacy.built) << legacy.error;
    EXPECT_FALSE (legacy.published) << "the legacy lane publishes nothing";

    // Identity, derived scope and the station list match the legacy lane bit for bit.
    EXPECT_EQ (unified.revision, legacy.revision);
    EXPECT_EQ (unified.result.region, legacy.result.region);
    EXPECT_EQ (unified.result.stations, legacy.result.stations);
    ASSERT_FALSE (unified.result.stations.empty ());

    // The transitional consumer mirror carries the band-lane contract fields.
    EXPECT_EQ (unified.result.build.inputPointCount, legacy.result.build.inputPointCount);
    EXPECT_EQ (unified.result.build.owners.size (), unified.result.build.points.size () / 3);
    ASSERT_EQ (unified.result.vertexOperations.size (), unified.result.build.owners.size ());
    EXPECT_FALSE (unified.result.build.lines.empty ()) << "band design lines convert into the mirror";
    EXPECT_EQ (unified.result.meshProvenance.domains, legacy.result.meshProvenance.domains);

    // Surface parity on a 0.25 m grid over the rebuild region: the unified and
    // hybrid kernels densify the same band stations differently (the documented
    // transition-density class, 0.244 m on the #82 fixture).
    const auto maxError =
        MaxSampledHeightError (unified.result.build.points, unified.result.build.triangles, legacy.result.build.points,
                               legacy.result.build.triangles, unified.result.region, error);
    ASSERT_GE (maxError, 0) << error;
    EXPECT_LE (maxError, 0.25) << "measured sampled error " << maxError;

    // Displayed totals keep the 0.25 m convention on both lanes.
    EXPECT_NEAR (unified.result.build.site.fill, legacy.result.build.site.fill, 20.0);
    EXPECT_NEAR (unified.result.build.site.cut, legacy.result.build.site.cut, 20.0);
    // The published snapshot is the mirror's authority.
    EXPECT_EQ (unified.published->points, unified.result.build.points);
    EXPECT_EQ (unified.published->displaySite.fill, unified.result.build.site.fill);
}

// #112 P4c: cover trimming runs in the unified lane with the legacy contract —
// a path leaving the terrain keeps only the longest covered station run.
TEST (EditTerrainDesignJob, BandPathLeavingTheTerrainTrimsIdenticallyToTheLegacyLane)
{
    DesignFixture fixture;
    std::string error;
    ASSERT_TRUE (
        edit::MovePathPoint (fixture.doc, fixture.path, fixture.doc.paths[0].points[0].id, { -30, 0, 0 }, error))
        << error;
    fixture.Capture ();
    TerrainDesignJob job;
    job.doc = std::make_shared<const edit::Document> (fixture.doc);
    job.capture = Capture (fixture);
    job.options = fixture.Options ();
    const auto unified = RunLandscapeTerrainDesign (job);
    ASSERT_TRUE (unified.built) << unified.error;
    const auto legacy = RunTerrainDesign (job);
    ASSERT_TRUE (legacy.built) << legacy.error;
    // Both lanes trimmed to the same covered run.
    EXPECT_EQ (unified.result.stations, legacy.result.stations);
    EXPECT_GT (unified.result.stations.front (), 9.0) << "stations off the terrain are dropped";
    EXPECT_EQ (unified.result.stations.size (), legacy.result.stations.size ());
    const auto maxError =
        MaxSampledHeightError (unified.result.build.points, unified.result.build.triangles, legacy.result.build.points,
                               legacy.result.build.triangles, unified.result.region, error);
    ASSERT_GE (maxError, 0) << error;
    EXPECT_LE (maxError, 0.25) << "measured sampled error " << maxError;
}

// #112 P4c: the two documented dispatch exceptions keep the legacy lane —
// outline grading (no unified equivalent yet) and a request naming neither a
// region target nor a path.
// #149 T1 (owner decision 2026-11-07): an outline request runs on the unified build; the
// legacy outline lane is its parity oracle. With no region the unified build rebuilds the
// lane's own region with its own face lines, spots and paved supports, so the two meshes are
// the same and every height agrees. Only a request with no target keeps the legacy lane.
TEST (EditTerrainDesignJob, AnOutlineRunsOnTheUnifiedBuildAndMatchesTheLegacyOracle149)
{
    DesignFixture fixture;
    TerrainDesignPublisher publisher;
    std::string error;
    TerrainDesignJob job;
    auto outline = fixture.Options ();
    outline.outline = true;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), outline, {}, job, error)) << error;
    const auto outlined = RunLandscapeTerrainDesign (job);
    const auto legacyOutline = RunTerrainDesign (job);
    ASSERT_TRUE (legacyOutline.built) << legacyOutline.error;
    ASSERT_TRUE (outlined.built) << outlined.error;
    EXPECT_TRUE (outlined.published) << "one unified publication";
    EXPECT_FALSE (outlined.regions);
    const auto& a = outlined.result.build;
    const auto& b = legacyOutline.result.build;
    EXPECT_EQ (a.triangles, b.triangles);
    ASSERT_EQ (a.points.size (), b.points.size ());
    double worst = 0;
    for (size_t i = 0; i < a.points.size (); ++i)
        worst = std::max (worst, std::abs (a.points[i] - b.points[i]));
    EXPECT_LT (worst, 1e-9);
    EXPECT_EQ (outlined.unmet, legacyOutline.unmet);

    auto none = fixture.Options ();
    none.source = kNoId;
    TerrainDesignJob empty;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), none, {}, empty, error)) << error;
    const auto refused = RunLandscapeTerrainDesign (empty);
    EXPECT_FALSE (refused.built);
    EXPECT_EQ (refused.error, RunTerrainDesign (empty).error);
    EXPECT_TRUE (refused.result.build.points.empty ());
    EXPECT_FALSE (refused.published);
}

// #175: the field preview of an outline design, beside its build: its contours lie on the
// built mesh, its fill is the build's to within the grid, and it answers in a fraction of the
// build's time with no mesh.
TEST (EditTerrainDesignJob, TheFieldPreviewOfAnOutlineLiesOnItsBuild175)
{
    DesignFixture fixture;
    TerrainDesignPublisher publisher;
    std::string error;
    TerrainDesignJob job;
    auto outline = fixture.Options ();
    outline.outline = true;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), outline, {}, job, error)) << error;
    const auto started = std::chrono::steady_clock::now ();
    const auto built = RunLandscapeTerrainDesign (job);
    const double buildMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    ASSERT_TRUE (built.built) << built.error;
    UnifiedPreviewAsk ask;
    ask.step = 0.1;
    ask.interval = 0.5;
    const auto previewed = RunLandscapeTerrainPreview (job, ask);
    ASSERT_TRUE (previewed.built) << previewed.error;
    EXPECT_EQ (previewed.revision, fixture.doc.revision);
    const auto& preview = previewed.preview;
    ASSERT_FALSE (preview.lines.empty ());
    EXPECT_FALSE (preview.triangles.empty ());
    EXPECT_EQ (preview.interval, 0.5);
    // Every contour point against the built mesh: nearly all within the mesher's tolerance and
    // the grid's interpolation (the rest at the build region's edge, where its faces step).
    std::vector<double> queries;
    for (const auto& line : preview.lines)
        for (size_t i = 0; i + 2 < line.points.size (); i += 3)
            queries.insert (queries.end (), { line.points[i], line.points[i + 1] });
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    const auto& build = built.result.build;
    ASSERT_TRUE (SampleTerrain ({ build.points, build.triangles }, queries, .25, samples, summary, error)) << error;
    size_t close = 0, sampled = 0, at = 0;
    for (const auto& line : preview.lines)
        for (size_t i = 0; i + 2 < line.points.size (); i += 3, ++at)
            if (at < samples.size () && samples[at].onSurface) {
                ++sampled;
                close += std::abs (samples[at].height - line.level) < 0.03;
            }
    ASSERT_GT (sampled, 0U);
    EXPECT_GE (double (close) / double (sampled), 0.95) << close << " of " << sampled;
    EXPECT_NEAR (preview.fill, build.site.fill, 0.05 * build.site.fill + 1.0);
    std::cout << "Field preview " << previewed.milliseconds << " ms (build " << buildMs << " ms), "
              << preview.lines.size () << " lines, " << close << "/" << sampled << " within 3 cm, fill " << preview.fill
              << " vs " << build.site.fill << std::endl;
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

TEST (VirtualEditorDebug, ExportedTerrainReplaysTheSameMeshOffline)
{
    for (bool nativeSolid : { false, true }) {
        TerrainHostFixture fixture;
        auto mesh = std::make_shared<geomsrv::Mesh> (*fixture.mesh);
        if (nativeSolid) {
            // Keep the entire native solid in the dump, not just its worker-compacted upward surface.
            for (size_t i = 0; i < 12; i += 3)
                mesh->vertices.insert (mesh->vertices.end (), { mesh->vertices[i], mesh->vertices[i + 1], -10 });
            const std::vector<uint32_t> shell { 4, 6, 5, 4, 7, 6, 0, 5, 1, 0, 4, 5, 1, 6, 2,
                                                1, 5, 6, 2, 7, 3, 2, 6, 7, 3, 4, 0, 3, 7, 4 };
            mesh->triangles.insert (mesh->triangles.end (), shell.begin (), shell.end ());
            mesh->triSourceBody.assign (12, 1);
            mesh->triSourcePgon.assign (12, 2);
            mesh->triMaterial.assign (12, 3);
            mesh->triSurfaceAttribute.assign (12, 4);
        }
        eh::Session session (fixture.doc);
        session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
        ASSERT_TRUE (eh::StartTerrainDesign (session, { mesh, nativeSolid, {} }, fixture.path));
        layers::Layer layer;
        ASSERT_TRUE (AwaitTerrain (session, layer));
        const auto* current = eh::CurrentTerrainDesign (session);
        ASSERT_NE (current, nullptr);
        const auto before = session.edits.Doc ();
        const auto depth = session.edits.UndoDepth ();
        std::string text, error;
        ASSERT_TRUE (eh::EncodeDebugScene (session, text, error)) << error;
        const auto parsed = evp::nodegraph::json::Parse (text);
        ASSERT_TRUE (parsed.ok) << parsed.error;
        const auto scene = debugreplay::Read (parsed.value);
        EXPECT_EQ (scene.doc.paths, before.paths);
        EXPECT_EQ (scene.doc.entities, before.entities);
        EXPECT_EQ (scene.doc.landscape, before.landscape);
        EXPECT_EQ (scene.doc.nextId, before.nextId);
        EXPECT_EQ (scene.doc.revision, before.revision);
        EXPECT_EQ (scene.baseline.vertices, mesh->vertices);
        EXPECT_EQ (scene.baseline.triangles, mesh->triangles);
        EXPECT_EQ (scene.baseline.triSourceBody, mesh->triSourceBody);
        EXPECT_EQ (scene.baseline.triSourcePgon, mesh->triSourcePgon);
        EXPECT_EQ (scene.baseline.triMaterial, mesh->triMaterial);
        EXPECT_EQ (scene.baseline.triSurfaceAttribute, mesh->triSurfaceAttribute);
        EXPECT_EQ (scene.nativeSolid, nativeSolid);
        const auto replayed = debugreplay::Run (scene);
        ASSERT_TRUE (replayed.built) << replayed.error;
        EXPECT_EQ (replayed.result.build.points, current->result.build.points);
        EXPECT_EQ (replayed.result.build.triangles, current->result.build.triangles);
        EXPECT_EQ (replayed.unmet, current->unmet);
        EXPECT_EQ (session.edits.UndoDepth (), depth);
        EXPECT_EQ (session.edits.Doc ().landscape, before.landscape);
    }
}

TEST (VirtualEditorDebug, EditedSceneNeverExportsAnOlderOutcomeAndRetainsStaleBaseline)
{
    TerrainHostFixture fixture;
    bool fresh = true;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::StartTerrainDesign (session, { fixture.mesh, false, [&] { return fresh; } }, fixture.path));
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    ASSERT_TRUE (session.edits.Apply ([&] (edit::Document& doc, std::string& error) {
        return edit::EditPath (
            doc, fixture.path,
            [] (edit::Path& path, std::string&) {
                for (auto& point : path.profile)
                    point.z += 0.02;
                return true;
            },
            error);
    }));
    std::string text, error;
    ASSERT_TRUE (eh::EncodeDebugScene (session, text, error)) << error;
    auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok);
    EXPECT_TRUE (parsed.value.Find ("outcome")->IsNull ());
    EXPECT_EQ (debugreplay::Read (parsed.value).baseline.vertices, fixture.mesh->vertices);
    fresh = false;
    ASSERT_TRUE (eh::EncodeDebugScene (session, text, error)) << error;
    parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok);
    bool baselineFresh = true;
    ASSERT_TRUE (parsed.value.Find ("baselineFresh")->AsBool (baselineFresh));
    EXPECT_FALSE (baselineFresh);
    EXPECT_TRUE (parsed.value.Find ("outcome")->IsNull ());
    EXPECT_EQ (debugreplay::Read (parsed.value).baseline.vertices, fixture.mesh->vertices);
}

TEST (VirtualEditorDebug, TerrainCanBeDumpedBeforeAnyPathExists)
{
    TerrainHostFixture fixture;
    auto doc = fixture.doc;
    doc.paths.clear ();
    eh::Session session (doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::SetTerrainBaseline (session, { fixture.mesh, false, {} }, 0));
    std::string text, error;
    ASSERT_TRUE (eh::EncodeDebugScene (session, text, error)) << error;
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok);
    EXPECT_TRUE (debugreplay::Read (parsed.value).doc.paths.empty ());
    EXPECT_EQ (debugreplay::Read (parsed.value).baseline.vertices, fixture.mesh->vertices);
}

TEST (VirtualEditorDebug, ChangingGradingModeDoesNotPairNewOptionsWithThePreviousMesh)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path));
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto revision = session.edits.Doc ().revision;
    eh::SetTerrainBand (session, true);
    EXPECT_EQ (session.edits.Doc ().revision, revision);
    std::string text, error;
    ASSERT_TRUE (eh::EncodeDebugScene (session, text, error)) << error;
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok);
    EXPECT_TRUE (parsed.value.Find ("outcome")->IsNull ());
    EXPECT_FALSE (debugreplay::Read (parsed.value).options.outline);
}

TEST (VirtualEditorDebug, RefusedDesignStillExportsItsInputsAndReplaysItsError)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path));
    ASSERT_TRUE (session.edits.Apply ([&] (edit::Document& doc, std::string& error) {
        return edit::EditPath (
            doc, fixture.path,
            [] (edit::Path& path, std::string&) {
                for (auto& point : path.points)
                    point.p.x += 500;
                return true;
            },
            error);
    }));
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer, true));
    const auto* current = eh::CurrentTerrainDesign (session);
    ASSERT_NE (current, nullptr);
    ASSERT_FALSE (current->built);
    ASSERT_FALSE (current->error.empty ());
    std::string text, error;
    ASSERT_TRUE (eh::EncodeDebugScene (session, text, error)) << error;
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok);
    const auto scene = debugreplay::Read (parsed.value);
    const auto replayed = debugreplay::Run (scene);
    EXPECT_FALSE (replayed.built);
    EXPECT_EQ (replayed.error, current->error);
    EXPECT_EQ (replayed.unmet, current->unmet);
}

TEST (VirtualEditorDebug, ReplayDumpFromEnvironment)
{
#ifdef _WIN32
    const auto* path = _wgetenv (L"TAPIOCA_VIRTUAL_EDITOR_REPLAY");
#else
    const auto* path = std::getenv ("TAPIOCA_VIRTUAL_EDITOR_REPLAY");
#endif
    if (!path || !*path)
        GTEST_SKIP () << "Set TAPIOCA_VIRTUAL_EDITOR_REPLAY to an Export debug scene JSON file.";
    const auto file = std::filesystem::path (path);
    ASSERT_LE (std::filesystem::file_size (file), 512ULL * 1024 * 1024);
    std::ifstream input (file, std::ios::binary);
    ASSERT_TRUE (input);
    const std::string text { std::istreambuf_iterator<char> (input), {} };
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    const auto scene = debugreplay::Read (parsed.value);
    const auto paved = edit::Pave (scene.doc.paths);
    std::cout << "Replay paths=" << scene.doc.paths.size () << " pavedTriangles=" << paved.triangles.size () / 3
              << " warning=" << paved.warning << '\n';
    for (const auto& corner : paved.corners)
        std::cout << "Corner path=" << corner.path << " point=" << corner.key.point << " other=" << corner.key.other
                  << " side=" << int (corner.key.side) << " otherSide=" << int (corner.key.otherSide)
                  << " radius=" << corner.key.radius << " max=" << corner.maxRadius
                  << " requested=" << corner.requestedRadius << " at=" << corner.at.x << ',' << corner.at.y << '\n';
    bool active = false;
    ASSERT_TRUE (parsed.value.Find ("designActive")->AsBool (active));
    if (!active)
        return; // A path-only/terrain-first dump is still useful for fillet diagnosis.
    {
        // #175: the same scene's field preview, timed beside the build.
        auto capture = std::make_shared<TerrainCapture> ();
        std::string captureError;
        if (CaptureLandscapeTerrain (scene.doc, scene.baseline, { scene.options.object, 2, 1 }, *capture,
                                     captureError)) {
            TerrainDesignJob job;
            job.doc = std::make_shared<const geomsrv::edit::Document> (scene.doc);
            job.capture = capture;
            job.options = scene.options;
            job.surfaceTable = scene.nativeSolid ? 5 : 0;
            // Twice through one outline cache, as the editor asks at rest after a drag (#176):
            // the first builds the outline geometry, the second reuses it; the second also
            // passes the first's sizes, so it calls Rust once.
            OutlineGeometryCache outlines;
            UnifiedPreviewAsk ask;
            for (const char* pass : { "first", "again" }) {
                const auto previewed = RunLandscapeTerrainPreview (job, ask, &outlines);
                std::cout << "Replay preview " << pass << " built=" << previewed.built
                          << " ms=" << previewed.milliseconds
                          << " triangles=" << previewed.preview.triangles.size () / 3
                          << " lines=" << previewed.preview.lines.size () << " fill=" << previewed.preview.fill
                          << " cut=" << previewed.preview.cut << " error=" << previewed.error << std::endl;
                ask.points = previewed.preview.points.size ();
                ask.triangles = previewed.preview.triangles.size ();
                ask.lines = previewed.preview.lines.size ();
                ask.linePoints = 0;
                for (const auto& line : previewed.preview.lines)
                    ask.linePoints += line.points.size ();
            }
        }
    }
    const auto replayStarted = std::chrono::steady_clock::now ();
    const auto outcome = debugreplay::Run (scene);
    std::cout << "Replay build ms="
              << std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - replayStarted).count ()
              << std::endl;
    std::cout << "Replay built=" << outcome.built << " triangles=" << outcome.result.build.triangles.size () / 3
              << " error=" << outcome.error << '\n';
    for (const auto& condition : outcome.unmet)
        std::cout << "Unmet: " << condition << '\n';
    // Optional: the replayed mesh as flat JSON arrays, for plotting it against the saved one.
#ifdef _WIN32
    const auto* out = _wgetenv (L"TAPIOCA_VIRTUAL_EDITOR_REPLAY_OUT");
#else
    const auto* out = std::getenv ("TAPIOCA_VIRTUAL_EDITOR_REPLAY_OUT");
#endif
    if (out && *out) {
        std::ofstream file { std::filesystem::path (out), std::ios::binary };
        ASSERT_TRUE (file);
        file.precision (17);
        const auto write = [&file] (const char* name, const auto& values, bool last) {
            file << '"' << name << "\":[";
            for (size_t i = 0; i < values.size (); ++i)
                file << (i ? "," : "") << values[i];
            file << (last ? "]" : "],");
        };
        file << '{';
        write ("points", outcome.result.build.points, false);
        write ("base", outcome.result.build.base, false);
        write ("triangles", outcome.result.build.triangles, false);
        file << "\"unmet\":" << outcome.unmet.size () << '}';
    }
    const auto* saved = parsed.value.Find ("outcome");
    if (!saved || saved->IsNull ())
        return; // Pending exports have inputs, not an invented expected output.
    const debugreplay::lj::Reader expected (*saved, "outcome");
    EXPECT_EQ (outcome.built, expected.Field ("built").Flag ());
    EXPECT_EQ (outcome.error, expected.Field ("error").Text ());
    const auto savedPoints = debugreplay::Buffer<double> (expected.Field ("points"));
    if (savedPoints.size () == outcome.result.build.points.size ()) {
        double maxDelta = 0;
        size_t different = 0;
        for (size_t i = 0; i < savedPoints.size (); ++i) {
            const double delta = std::abs (savedPoints[i] - outcome.result.build.points[i]);
            maxDelta = std::max (maxDelta, delta);
            different += delta > 0;
        }
        std::cout << "Replay differing coordinates=" << different << " max delta=" << maxDelta << '\n';
    }
    EXPECT_EQ (outcome.result.build.points, debugreplay::Buffer<double> (expected.Field ("points")));
    EXPECT_EQ (outcome.result.build.triangles, debugreplay::Buffer<uint32_t> (expected.Field ("triangles")));
}

TEST (HudTerrainDesign, LifecycleLogsDistinguishCaptureBuildPublicationAndLayerWithoutPerFrameSpam)
{
    geomsrv::archviz::test::LogCapture log;
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    other.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::RequestTerrainCapture (other, true)) << other.message;
    EXPECT_TRUE (other.terrainCapture);
}

TEST (HudTerrainDesign, ATerrainSetBeforeItsPathWaitsAndTheDesignStartsWhenOneIsChosen)
{
    TerrainHostFixture fixture;
    fixture.doc.landscape->propertyLineSourceId = 0;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true;          // the mesh build lane (#175: the editor previews)
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

TEST (EditTerrainOutline, CapturedPathPlacementsKeepRemoteTerrainAtItsBaseline)
{
    // User exports 08:20:08 / 08:20:23 (visually correct) / 08:20:42 UTC, 2026-10-28.
    // Only durable region history is omitted from the fixture; all paths/profiles/kerbs are retained.
    std::ifstream file (std::filesystem::path (EVP_TEST_FIXTURE_DIR) / "path-placement-20261028.document.json");
    const std::string text { std::istreambuf_iterator<char> (file), {} };
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    edit::Document original;
    debugreplay::lj::ReadDocument ({ parsed.value, "document" }, original, LandscapeStorage::Virtual);
    constexpr double side = 17.96522521070513, base = -0.19999999999999996;
    const std::vector<double> points { 0, 0, base, side, 0, base, side, side, base, 0, side, base };
    const std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
    const ProvenanceRequest provenance;
    for (const edit::Vec3 shift : { edit::Vec3 { 0, 0, 0 },
                                    { -1.402718185676473, -0.285991086400059, 0 },
                                    { 0.4221773180191345, 7.50386136221105, 0 } }) {
        for (double perturb : { -0.02, 0.0, 0.02 }) {
            auto doc = original;
            for (auto& path : doc.paths)
                for (auto& point : path.points)
                    point.p = point.p + shift + edit::Vec3 { perturb, 0, 0 };
            const auto paved = edit::Pave (doc.paths);
            ASSERT_TRUE (paved.warning.empty ()) << paved.warning;
            const auto distance = [&] (edit::Vec3 p) {
                double best = std::numeric_limits<double>::infinity ();
                for (const auto& ring : paved.rings)
                    for (size_t i = 0; i < ring.size (); ++i) {
                        const auto a = ring[i], b = ring[(i + 1) % ring.size ()], d = b - a;
                        const double length2 = d.x * d.x + d.y * d.y;
                        const double t =
                            length2 > 0 ? std::clamp (((p.x - a.x) * d.x + (p.y - a.y) * d.y) / length2, 0.0, 1.0) : 0;
                        best = std::min (best, edit::DistanceXY (p, a + d * t));
                    }
                return best;
            };
            std::vector<double> queries;
            for (double x = 0.5; x < side; x += 0.5)
                for (double y = 0.5; y < side; y += 0.5)
                    if (distance ({ x, y, 0 }) > 3)
                        queries.insert (queries.end (), { x, y });
            ASSERT_FALSE (queries.empty ());
            TerrainDesignResult result;
            uint32_t unreached = 0;
            std::string error;
            ASSERT_TRUE (EvaluateOutlineDesign (doc, { { points, triangles }, provenance, doc.revision, {} }, {},
                                                result, unreached, error))
                << error;
            EXPECT_EQ (unreached, 0u);
            std::vector<TerrainSample> samples;
            TerrainFieldSummary summary;
            ASSERT_TRUE (
                SampleTerrain ({ result.build.points, result.build.triangles }, queries, 0.25, samples, summary, error))
                << error;
            double maxError = 0;
            for (const auto& sample : samples) {
                ASSERT_TRUE (sample.onSurface);
                maxError = std::max (maxError, std::abs (sample.height - base));
            }
            EXPECT_LT (maxError, 1e-5) << "shift " << shift.x << ", " << shift.y << "; perturb " << perturb;
        }
    }
}

TEST (EditTerrainOutline, ABentFillPathGradesSmoothFacesAndReportsTheStepAtTheTerrainsEdge)
{
    // User export 12:41:02 UTC, 2026-11-04: an L path in fill from 1.2 m to 0 over a level
    // 18 m slab, its start 2 m from the slab's edge. The nearest-point faces jumped where the
    // two legs' reaches met inside the bend and cliffed silently at the edge.
    constexpr double side = 17.96522521070513, base = -0.19999999999999996;
    const std::vector<double> points { 0, 0, base, side, 0, base, side, side, base, 0, side, base };
    const std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
    edit::AddPath (doc,
                   { { 2.755713478079103, 8.275542496185775, 1.1977159969400257 },
                     { 8.611676648767107, 11.656484409147028, 0 },
                     { 9.3397481833684, 6.058714464646998, 0 } },
                   1.5);
    TerrainDesignResult result;
    uint32_t unreached = 0, clipped = 0;
    ASSERT_TRUE (EvaluateOutlineDesign (doc, { { points, triangles }, ProvenanceRequest {}, doc.revision, {} }, {},
                                        result, unreached, error, &clipped))
        << error;
    EXPECT_EQ (unreached, 0u);
    EXPECT_GT (clipped, 0u) << "the fill face needs ~4 m beside the start and has 2 m";
    // Inside the bend and away from the slab's edge, a 0.1 m grid never climbs faster than
    // the steeper (cut, 1 in 2) grade plus the interpolation over the 0.5 m rings.
    const size_t rows = 51;
    std::vector<double> queries;
    for (size_t i = 0; i < 31; ++i)
        for (size_t j = 0; j < rows; ++j)
            queries.insert (queries.end (), { 6.0 + 0.1 * double (i), 5.0 + 0.1 * double (j) });
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    ASSERT_TRUE (
        SampleTerrain ({ result.build.points, result.build.triangles }, queries, 0.25, samples, summary, error))
        << error;
    const auto paved = edit::Pave (doc.paths);
    const auto onPaved = [&] (double x, double y) {
        for (const auto& ring : paved.rings) {
            Clipper2Lib::PathD path;
            for (const auto& p : ring)
                path.push_back ({ p.x, p.y });
            if (Clipper2Lib::PointInPolygon (Clipper2Lib::PointD { x, y }, path) !=
                Clipper2Lib::PointInPolygonResult::IsOutside)
                return true;
        }
        return false;
    };
    double steepest = 0;
    for (size_t i = 0; i + rows < samples.size (); ++i) {
        for (const size_t j : { i + 1, i + rows }) {
            if ((j == i + 1 && (i + 1) % rows == 0) || onPaved (queries[i * 2], queries[i * 2 + 1]) ||
                onPaved (queries[j * 2], queries[j * 2 + 1]))
                continue;
            steepest = std::max (steepest, std::abs (samples[j].height - samples[i].height) / 0.1);
        }
    }
    EXPECT_LT (steepest, 0.6) << "a face inside the bend is steeper than its grade";
}

TEST (EditTerrainOutline, EachPathAndRangeGradesItsFacesByItsOwnSlopes)
{
    // #151: two parallel paths 2 m above a level 40 x 20 m slab. A's first range leaves at
    // 50 %, its second (from station 18) at 25 %; B leaves at 25 %. Every face keeps its own
    // section's grade, whichever path is the design path.
    const std::vector<double> points { 0, 0, 0, 40, 0, 0, 40, 20, 0, 0, 20, 0 };
    const std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
    const auto a = edit::AddPath (doc, { { 2, 5, 2 }, { 38, 5, 2 } }, 1.5);
    const auto b = edit::AddPath (doc, { { 2, 15, 2 }, { 38, 15, 2 } }, 1.5);
    const auto slopes = [&] (edit::Id id, double first, double second) {
        return edit::EditPath (
            doc, id,
            [first, second] (edit::Path& path, std::string&) {
                auto& range = path.sections.front ();
                range.rounding = 0;
                range.fillSlope = { TerrainSlope::Unit::Percent, first, 0 };
                if (second > 0) {
                    auto next = range;
                    next.id = range.id + 1000;
                    next.station = 18;
                    next.fillSlope = { TerrainSlope::Unit::Percent, second, 0 };
                    path.sections.push_back (next);
                }
                return true;
            },
            error);
    };
    ASSERT_TRUE (slopes (a, 50, 25)) << error;
    ASSERT_TRUE (slopes (b, 25, 0)) << error;
    // Per side, the fall from 1 m to 2 m beyond the paved edge (half width 0.75 m).
    const auto fall = [&] (const TerrainDesignResult& result, double x, double edgeY, double outward) {
        const std::vector<double> queries { x, edgeY + outward, x, edgeY + 2 * outward };
        std::vector<TerrainSample> samples;
        TerrainFieldSummary summary;
        EXPECT_TRUE (
            SampleTerrain ({ result.build.points, result.build.triangles }, queries, 0.25, samples, summary, error))
            << error;
        return samples.size () == 2 ? samples[0].height - samples[1].height : NAN;
    };
    for (const auto design : { a, b }) {
        TerrainDesignResult result;
        uint32_t unreached = 0;
        const auto options = OutlineOptionsFor (doc, design, 10);
        ASSERT_TRUE (EvaluateOutlineDesign (doc, { { points, triangles }, ProvenanceRequest {}, doc.revision, {} },
                                            options, result, unreached, error))
            << error;
        EXPECT_NEAR (fall (result, 10, 4.25, -1), 0.5, 1e-6) << "A's first range, design path " << design;
        EXPECT_NEAR (fall (result, 30, 4.25, -1), 0.25, 1e-6) << "A's second range, design path " << design;
        EXPECT_NEAR (fall (result, 20, 15.75, 1), 0.25, 1e-6) << "B, design path " << design;
    }
}

TEST (EditTerrainOutline, SubMetreRingLocationSurvivesWindingAndSurveyTranslations)
{
    namespace cp = Clipper2Lib;
    for (double offset : { 0.0, 1000000.0, -1000000.0 })
        for (bool reverse : { false, true }) {
            cp::PathD ring { { offset + 0.25, offset + 0.25 },
                             { offset + 0.75, offset + 0.25 },
                             { offset + 0.75, offset + 0.75 },
                             { offset + 0.25, offset + 0.75 } };
            if (reverse)
                std::reverse (ring.begin (), ring.end ());
            const ClipperPlanRing location (ring);
            EXPECT_EQ (location.Locate ({ offset + 0.5, offset + 0.5 }), cp::PointInPolygonResult::IsInside);
            EXPECT_EQ (location.Locate ({ offset + 0.74, offset + 0.5 }), cp::PointInPolygonResult::IsInside);
            EXPECT_EQ (location.Locate ({ offset + 0.75, offset + 0.5 }), cp::PointInPolygonResult::IsOn);
            EXPECT_EQ (location.Locate ({ offset + 0.76, offset + 0.5 }), cp::PointInPolygonResult::IsOutside);
        }
    EXPECT_EQ (ClipperPlanRing ({}).Locate ({ 0, 0 }), cp::PointInPolygonResult::IsOutside);
    const ClipperPlanRing invalid (
        cp::PathD { { std::numeric_limits<double>::infinity (), 0.0 }, { 1.0, 0.0 }, { 1.0, 1.0 } });
    EXPECT_EQ (invalid.Locate ({ 0, 0 }), cp::PointInPolygonResult::IsOutside);
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    EXPECT_EQ (eh::CurrentTerrainDesign (session), nullptr);  // Stale old build is never current,
    EXPECT_TRUE (eh::GetTerrainDesignStatus (session).stale); // only shown until the rebuild (#71).
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
                section.crossfall = edit::PercentCrossfall (0);
            }
            return true;
        },
        error))
        << error;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    session.terrainBand = true;      // the band's own lines and provenance
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    // The sharpest case is an edit that keeps the triangle count (a contour cache keyed on it
    // would keep the old contours): take the first small lift that does. Since the faces are
    // meshed to their error (#149 T3) even a micrometre may reshuffle the mesh, so when none
    // keeps the count the last lift stands, and the contours must still be the new mesh's.
    const TerrainDesignOutcome* current = nullptr;
    for (const double lift : { 1e-6, 1e-5, 1e-4, 0.0005, 0.001, 0.002, 0.005, 0.02 }) {
        ASSERT_TRUE (session.edits.Apply ([&] (edit::Document& doc, std::string&) {
            for (auto& grade : edit::FindPath (doc, fixture.path)->profile)
                grade.z += lift;
            return true;
        }));
        ASSERT_TRUE (AwaitTerrain (session, layer));
        current = eh::CurrentTerrainDesign (session);
        ASSERT_EQ (current, previous) << "publisher reuses its optional storage";
        if (current->result.build.triangles.size () == triangles)
            break;
    }
    if (current->result.build.triangles.size () != triangles)
        std::cout << "no lift kept the triangle count; the last one is checked" << std::endl;
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (session.terrainPath, fixture.path);
    const auto original = eh::CurrentTerrainDesign (session)->result.build.points;
    const auto section = edit::FindPath (session.edits.Doc (), fixture.path)->sections.front ().id;
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, section)) << session.message;
    session.terrainSectionDraft->value.widthLeft = 2;
    session.terrainSectionDraft->value.widthRight = 2;
    session.terrainSectionDraft->value.crossfall = edit::PercentCrossfall (1);
    session.terrainSectionDraft->value.fall = edit::Fall::Crown;
    session.terrainSectionDraft->value.cutSlope = { TerrainSlope::Unit::Percent, 60, 0 };
    const auto depth = session.edits.UndoDepth ();
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    EXPECT_EQ (session.edits.UndoDepth (), depth + 1);
    eh::Follow (session, layer);
    ASSERT_NE (TerrainMesh (layer), nullptr) << "the last build stays until the rebuild (#71)";
    EXPECT_EQ (TerrainMesh (layer)->points, original);
    EXPECT_TRUE (eh::GetTerrainDesignStatus (session).stale);
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_NE (eh::CurrentTerrainDesign (session)->result.build.points, original);
    EXPECT_GT (eh::CurrentTerrainDesign (session)->result.build.site.cut, 0);
    EXPECT_EQ (session.edits.UndoDepth (), depth + 1);
    ASSERT_TRUE (session.edits.Undo ());
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (eh::CurrentTerrainDesign (session)->result.build.points, original);
}

// #175 phase B: the editor shows the terrain from its field as it is edited and builds the mesh
// only when asked. Edits never start the mesher; Build mesh builds this revision once; the next
// edit is previewed again, without a build.
TEST (HudTerrainDesign, TheEditorPreviewsFromTheFieldAndMeshesOnlyWhenAsked175)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    ASSERT_FALSE (session.terrainAutoBuild) << "the editor's default";
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    const auto previewed = [&] {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
        while (std::chrono::steady_clock::now () < deadline) {
            eh::Follow (session, layer);
            const auto status = eh::GetTerrainDesignStatus (session);
            if (status.previewed && !status.previewPending)
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        return false;
    };
    ASSERT_TRUE (previewed ());
    auto status = eh::GetTerrainDesignStatus (session);
    EXPECT_FALSE (status.pending) << "no mesh build without Build mesh";
    EXPECT_EQ (eh::CurrentTerrainDesign (session), nullptr);
    ASSERT_NE (TerrainMesh (layer), nullptr) << "the field preview is drawn";
    EXPECT_GT (status.cut + status.fill, 0.0) << "quantities from the field";
    EXPECT_EQ (status.cut, status.previewCut);
    // An edit is previewed again; still no build.
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
    ASSERT_TRUE (session.edits.Apply (raise));
    ASSERT_TRUE (previewed ());
    EXPECT_FALSE (eh::GetTerrainDesignStatus (session).pending);
    // Build mesh: this revision's mesh, once.
    ASSERT_TRUE (eh::RequestTerrainBuild (session));
    ASSERT_TRUE (AwaitTerrain (session, layer));
    status = eh::GetTerrainDesignStatus (session);
    EXPECT_TRUE (status.built);
    EXPECT_FALSE (status.previewed) << "the built mesh is drawn";
    EXPECT_FALSE (eh::RequestTerrainBuild (session)) << "already built: " << session.message;
    // The next edit returns to the preview, without a build.
    ASSERT_TRUE (session.edits.Apply (raise));
    ASSERT_TRUE (previewed ());
    EXPECT_FALSE (eh::GetTerrainDesignStatus (session).pending);
}

// #175: a path-band design previews from the field as an outline does (the band over the
// composed ground), without a mesh build.
TEST (HudTerrainDesign, ABandDesignPreviewsFromTheFieldToo175)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainBand = true;
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    auto status = eh::GetTerrainDesignStatus (session);
    while (std::chrono::steady_clock::now () < deadline && !(status.previewed && !status.previewPending)) {
        eh::Follow (session, layer);
        status = eh::GetTerrainDesignStatus (session);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    ASSERT_TRUE (status.previewed) << status.error;
    EXPECT_FALSE (status.pending) << "no mesh build without Build mesh";
    ASSERT_NE (TerrainMesh (layer), nullptr);
    EXPECT_GT (status.cut + status.fill, 0.0) << "the band moves the ground";
}

TEST (HudTerrainDesign, EditsCoalesceUntilReleaseAndStaleWorkersNeverDraw)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    // No build runs through the gesture, so no stale build ever draws; what may draw is the
    // field preview (#175), which follows the document through it.
    for (int i = 0; i < 20; ++i) {
        eh::Follow (session, layer);
        if (TerrainMesh (layer))
            EXPECT_TRUE (eh::GetTerrainDesignStatus (session).previewed) << "only the field preview draws";
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    EXPECT_EQ (session.edits.UndoDepth (), depth);
    ASSERT_TRUE (session.edits.End ());
    ASSERT_TRUE (AwaitTerrain (session, layer));
    EXPECT_EQ (session.edits.UndoDepth (), depth + 1);
    ASSERT_NE (eh::CurrentTerrainDesign (session), nullptr);
    EXPECT_TRUE (eh::CurrentTerrainDesign (session)->result.conflicts.empty ());
}

TEST (HudTerrainDesign, PathsOnlyNeitherRebuildsNorDrawsTheTerrainUntilTurnedBackOn)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    ASSERT_NE (TerrainMesh (layer), nullptr);
    eh::SetTerrainLive (session, false);
    layer = {};
    eh::Follow (session, layer);
    EXPECT_EQ (TerrainMesh (layer), nullptr) << "paths only: the terrain is not drawn";
    // A path edit while paused starts no rebuild.
    ASSERT_TRUE (session.edits.Apply (
        [&] (edit::Document& doc, std::string& error) {
            return edit::EditPath (
                doc, fixture.path,
                [] (edit::Path& path, std::string&) {
                    for (auto& point : path.profile)
                        point.z += 0.1;
                    return true;
                },
                error);
        },
        true));
    for (int i = 0; i < 20; ++i) {
        layer = {};
        eh::Follow (session, layer);
        EXPECT_FALSE (eh::GetTerrainDesignStatus (session).pending) << "no rebuild runs while paused";
        EXPECT_EQ (TerrainMesh (layer), nullptr);
    }
    // Back on: it rebuilds from the paths as they are now.
    eh::SetTerrainLive (session, true);
    const auto revision = session.edits.Doc ().revision;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (eh::CurrentTerrainDesign (session)->revision != revision && std::chrono::steady_clock::now () < deadline) {
        eh::Follow (session, layer);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    EXPECT_EQ (eh::CurrentTerrainDesign (session)->revision, revision);
    EXPECT_NE (TerrainMesh (layer), nullptr);
}

// #112 P5: Author (inside StartTerrainDesign) is the only document-mutating
// step of a design. The poll-side worker start consumes the current immutable
// snapshot and never authors: the document revision, the landscape object and
// operation counts, nextId and the undo depth are all unchanged by a rebuild
// launch, and the new build still publishes against the edited revision.
TEST (HudTerrainDesign, AWorkerStartConsumesTheImmutableSnapshotAndNeverAuthors)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
    EXPECT_GT (session.edits.UndoDepth (), 0U) << "Authoring lands as one history step";
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto objects = session.edits.Doc ().landscape->objects.size ();
    const auto operations = session.edits.Doc ().landscape->operations.size ();
    const auto nextId = session.edits.Doc ().nextId;
    const auto generation = eh::CurrentTerrainDesign (session)->generation;
    // One document edit; the next idle poll coalesces it into one worker start.
    // The snapshot below is taken after the edit, so every assertion brackets
    // exactly the poll/worker start.
    ASSERT_TRUE (session.edits.Apply ([=] (edit::Document& value, std::string&) {
        edit::FindPath (value, fixture.path)->profile.front ().z += 1;
        return true;
    }));
    const auto revision = session.edits.Doc ().revision;
    const auto depth = session.edits.UndoDepth ();
    eh::PollTerrainDesign (session);
    EXPECT_EQ (session.edits.Doc ().revision, revision) << "a worker start authors nothing";
    EXPECT_EQ (session.edits.Doc ().landscape->objects.size (), objects);
    EXPECT_EQ (session.edits.Doc ().landscape->operations.size (), operations);
    EXPECT_EQ (session.edits.Doc ().nextId, nextId);
    EXPECT_EQ (session.edits.UndoDepth (), depth);
    // The launch really happened: the rebuild publishes a new generation for
    // the edited revision.
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    const tapioca::geometry::TerrainDesignOutcome* current = nullptr;
    while (std::chrono::steady_clock::now () < deadline) {
        eh::Follow (session, layer);
        current = eh::CurrentTerrainDesign (session);
        if (current && current->built && current->generation != generation)
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    ASSERT_NE (current, nullptr);
    ASSERT_TRUE (current->built) << current->error;
    EXPECT_NE (current->generation, generation);
    EXPECT_EQ (current->revision, revision);
}

TEST (HudTerrainDesign, APathDragKeepsTheLastBuiltSurfaceDrawnUnchangedUntilItsRebuildPublishes)
{
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    session.contourStep = 0.5;
    auto valid = std::make_shared<bool> (true);
    eh::TerrainDesignBaseline baseline { fixture.mesh, false, [valid] { return *valid; } };
    ASSERT_TRUE (eh::StartTerrainDesign (session, baseline, fixture.path)) << session.message;
    layers::Layer layer;
    ASSERT_TRUE (AwaitTerrain (session, layer));
    const auto built = TerrainMesh (layer)->points;
    const auto generation = eh::CurrentTerrainDesign (session)->generation;
    const auto style = TerrainMesh (layer)->style;
    size_t rings = 0; // The paths' outline shares the ring colour: count the built layer's.
    for (const auto& line : layer.polylines)
        rings += line.rgba == 0x37523DFFu;
    const auto lines = [] (const layers::Layer& value) {
        size_t ring = 0, contours = 0;
        for (const auto& line : value.polylines)
            ring += line.rgba == 0x37523DFFu, contours += line.rgba == 0x2F4A35E6u || line.rgba == 0x37523D99u;
        return std::pair { ring, contours };
    };
    // Never a blank frame and never faded (the user, 2026-10-30: fading it is distracting):
    // the last built surface as it was until the field preview of a newer revision lands
    // (#175), then that preview -- the same look, its own contours, no rebuild ring. Only the
    // status, and the panel reading it, say the build is behind.
    const auto stale = [&] {
        const auto* mesh = TerrainMesh (layer);
        ASSERT_NE (mesh, nullptr) << "no blank frame";
        const auto status = eh::GetTerrainDesignStatus (session);
        if (!status.previewed)
            EXPECT_EQ (mesh->points, built) << "the last built surface until a preview lands";
        EXPECT_EQ (mesh->style.opacity, style.opacity);
        EXPECT_EQ (mesh->style.occludedOpacity, style.occludedOpacity);
        const auto [ring, contours] = lines (layer);
        EXPECT_EQ (ring, status.previewed ? rings - 1 : rings) << "the preview has no rebuild ring";
        EXPECT_GT (contours, 0u);
        EXPECT_FALSE (status.current);
        EXPECT_TRUE (status.stale);
        EXPECT_EQ (eh::CurrentTerrainDesign (session), nullptr);
    };
    // Press: the gesture opens before anything moves.
    ASSERT_TRUE (session.edits.Begin ());
    session.publishedRevision = UINT64_MAX;
    ASSERT_TRUE (eh::Follow (session, layer));
    stale ();
    // Each move restarts from the gesture's start, so the pointer's travel grows per frame.
    const auto shift = [&] (double dy) {
        return [&fixture, dy] (edit::Document& doc, std::string& error) {
            return edit::EditPath (
                doc, fixture.path,
                [dy] (edit::Path& path, std::string&) {
                    for (auto& point : path.points)
                        point.p.y += dy;
                    return true;
                },
                error);
        };
    };
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE (session.edits.Move (shift (0.25 * i)));
        ASSERT_TRUE (eh::Follow (session, layer));
        stale ();
    }
    EXPECT_FALSE (eh::GetTerrainDesignStatus (session).pending) << "no rebuild per pointer move";
    // Release: the rebuild launches, and the stale surface holds until it publishes.
    ASSERT_TRUE (session.edits.End ());
    eh::Follow (session, layer);
    stale ();
    EXPECT_TRUE (eh::GetTerrainDesignStatus (session).pending);
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline && !eh::CurrentTerrainDesign (session)) {
        eh::Follow (session, layer);
        ASSERT_NE (TerrainMesh (layer), nullptr) << "no blank frame before publication";
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    eh::Follow (session, layer);
    ASSERT_NE (eh::CurrentTerrainDesign (session), nullptr);
    EXPECT_GT (eh::CurrentTerrainDesign (session)->generation, generation);
    ASSERT_NE (TerrainMesh (layer), nullptr);
    EXPECT_NE (TerrainMesh (layer)->points, built) << "replaced by the rebuild";
    EXPECT_EQ (lines (layer).first, rings);
    EXPECT_FALSE (eh::GetTerrainDesignStatus (session).stale);
    // A stale terrain capture hides even the last build, and keeps its error.
    ASSERT_TRUE (session.edits.Begin ());
    ASSERT_TRUE (session.edits.Move (shift (0.25)));
    *valid = false;
    eh::Follow (session, layer);
    EXPECT_EQ (TerrainMesh (layer), nullptr);
    const auto status = eh::GetTerrainDesignStatus (session);
    EXPECT_FALSE (status.stale);
    EXPECT_EQ (status.error.rfind ("TerrainCaptureStale", 0), 0U) << status.error;
    ASSERT_TRUE (session.edits.End ());
}

TEST (HudTerrainDesign, RefusedDesignDrawsLocatedPlacesAndStartRefusalsAreAtomic)
{
    geomsrv::archviz::test::LogCapture log;
    TerrainHostFixture fixture;
    eh::Session session (fixture.doc);
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
            for (auto& section : path.sections) {
                section.cutSlope = { TerrainSlope::Unit::Percent, 0.1, 0 };
                section.fillSlope = { TerrainSlope::Unit::Percent, 0.1, 0 };
            }
            return true;
        },
        error));
    eh::Session refused (fixture.doc);
    refused.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
    refused.terrainBand = true;      // located refusals are the band's
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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
    session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
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

// #175: paving a straight path every half metre must not change its built terrain. A split edge
// used to lift its own collinear neighbours as though they met at a crease; compare the complete
// Rust build through its C++ facade with the same pavement as four whole edges.
TEST (EditTerrainOutline, HalfMetreSplitPavementBuildsLikeTheSameFourWholeEdges)
{
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    for (uint32_t row = 0; row < 13; ++row)
        for (uint32_t column = 0; column < 17; ++column)
            points.insert (points.end (), { -20.5 + 5.0 * column, -30.5 + 5.0 * row, 10.0 });
    for (uint32_t row = 0; row < 12; ++row)
        for (uint32_t column = 0; column < 16; ++column) {
            const uint32_t a = row * 17 + column;
            triangles.insert (triangles.end (), { a, a + 1, a + 18, a, a + 18, a + 17 });
        }
    const auto height = [] (double x) { return 12.0 + 0.02 * x; };
    constexpr uint32_t count = 81;
    std::vector<double> split;
    for (const double side : { -2.0, 2.0 })
        for (uint32_t i = 0; i < count; ++i) {
            const double x = 0.5 * i;
            split.insert (split.end (), { x, side, height (x) });
        }
    std::vector<uint32_t> splitTriangles;
    for (uint32_t i = 0; i + 1 < count; ++i)
        splitTriangles.insert (splitTriangles.end (), { i, i + 1, count + i + 1, i, count + i + 1, count + i });
    const std::vector<double> whole { 0, -2, height (0), 40, -2, height (40), 40, 2, height (40), 0, 2, height (0) };
    const std::vector<uint32_t> wholeTriangles { 0, 1, 2, 0, 2, 3 };

    // The editor's two open outline halves, identically sampled in both requests. The surface
    // triangulation is the only difference; the sampled spots make their output heights comparable.
    std::vector<std::vector<double>> lines (2);
    const std::array<std::array<double, 2>, 3> first { { { 0, -2 }, { 40, -2 }, { 40, 2 } } };
    const std::array<std::array<double, 2>, 3> second { { { 40, 2 }, { 0, 2 }, { 0, -2 } } };
    for (const auto [half, corners] : { std::pair { 0U, &first }, { 1U, &second } })
        for (size_t edge = 0; edge + 1 < corners->size (); ++edge) {
            const auto& a = corners->at (edge);
            const auto& b = corners->at (edge + 1);
            const auto pieces = static_cast<uint32_t> (std::ceil (std::hypot (b[0] - a[0], b[1] - a[1]) / 0.5));
            for (uint32_t i = 0; i <= pieces; ++i) {
                if (edge > 0 && i == 0)
                    continue;
                const double t = static_cast<double> (i) / pieces;
                lines[half].insert (lines[half].end (), { a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), 0 });
            }
        }
    std::vector<double> spots;
    for (const double x : { 3.0, 9.7, 10.0, 10.3, 19.9, 20.0, 29.5, 30.0, 37.0 })
        for (const double y : { -5.0, -4.0, -3.0, -2.5, 2.5, 3.0, 4.0, 5.0 })
            spots.insert (spots.end (), { x, y, 0.0 });
    const auto run = [&] (const std::vector<double>& surface, const std::vector<uint32_t>& surfaceTriangles) {
        OutlineTerrainRequest request;
        request.mesh = { points, triangles };
        request.region = kRegion;
        request.surface = { surface, surfaceTriangles };
        request.lines = lines;
        request.spots = spots;
        request.cut = { TerrainSlope::Unit::Percent, 50, 0 };
        request.fill = request.cut;
        request.limit = 10;
        request.rounding = 1;
        request.tolerance = 1e-6;
        OutlineTerrainResult built;
        std::string error;
        EXPECT_TRUE (BuildOutlineTerrain (request, built, error)) << error;
        return built;
    };
    const auto dense = run (split, splitTriangles);
    const auto sparse = run (whole, wholeTriangles);
    ASSERT_FALSE (dense.points.empty ());
    ASSERT_FALSE (sparse.points.empty ());
    const auto heights = [] (const OutlineTerrainResult& built) {
        std::map<std::pair<double, double>, double> at;
        for (size_t i = 0; i + 2 < built.points.size (); i += 3)
            at[{ built.points[i], built.points[i + 1] }] = built.points[i + 2];
        return at;
    };
    const auto denseHeights = heights (dense), sparseHeights = heights (sparse);
    size_t compared = 0;
    for (size_t i = 0; i + 2 < spots.size (); i += 3) {
        const std::pair at { spots[i], spots[i + 1] };
        const auto a = denseHeights.find (at), b = sparseHeights.find (at);
        ASSERT_NE (a, denseHeights.end ());
        ASSERT_NE (b, sparseHeights.end ());
        EXPECT_NEAR (a->second, b->second, 1e-9) << at.first << ", " << at.second;
        ++compared;
    }
    EXPECT_EQ (compared, spots.size () / 3);
}

TEST (EditTerrainOutline, ACoplanarTerrainEdgeLeavesNoSliversAcrossTheFaces)
{
    // User export 17:26:24 UTC, 2026-11-05: a level two-triangle terrain whose diagonal runs past
    // the path's start. Kept as a constraint, the offset rings crossed it at a shallow angle and
    // left slivers along it; through nearly collinear points on the rounded faces their planes
    // tilt steeply, a dent in the shading under correct contours.
    const std::vector<double> points { -3.16, -3.24, -0.2, 25.3, -3.24, -0.2, 25.3, 25.21, -0.2, -3.16, 25.21, -0.2 };
    const std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
    edit::AddPath (doc,
                   { { 2.91, 6.03, 2.178 },
                     { 5.02, 16.53, 1.7 },
                     { 12.82, 5.33, 1.0 },
                     { 16.38, 17.31, 0.5 },
                     { 22.58, 3.91, 0.0 } },
                   1.5);
    TerrainDesignResult result;
    uint32_t unreached = 0;
    ASSERT_TRUE (EvaluateOutlineDesign (doc, { { points, triangles }, ProvenanceRequest {}, doc.revision, {} }, {},
                                        result, unreached, error))
        << error;
    const auto& p = result.build.points;
    const auto& t = result.build.triangles;
    size_t slivers = 0;
    for (size_t i = 0; i + 2 < t.size (); i += 3) {
        const auto at = [&] (size_t k) {
            return edit::Vec3 { p[t[i + k] * 3], p[t[i + k] * 3 + 1], p[t[i + k] * 3 + 2] };
        };
        const edit::Vec3 a = at (0), b = at (1), c = at (2);
        const edit::Vec3 u = b - a, v = c - a;
        const double nx = u.y * v.z - u.z * v.y, ny = u.z * v.x - u.x * v.z, nz = u.x * v.y - u.y * v.x;
        // The faces' own triangles: not the step where they meet the terrain's edge, and not the
        // paved outline's zero-area triples of points along one straight kerb.
        const auto inland = [] (edit::Vec3 q) { return q.x > -2.6 && q.x < 24.7 && q.y > -2.7 && q.y < 24.6; };
        if (std::abs (nz) < 2e-4 || !inland (a) || !inland (b) || !inland (c))
            continue;
        const double slope = std::hypot (nx, ny) / std::abs (nz);
        // No sliver across the faces tilts past 4/3 of their grade (the rounded top); the cut
        // grade is 1:2. The dent this guards against was long slivers through the rounded faces
        // along the terrain's diagonal. Since the kernel's mesher (#149 T3) meshes the faces to a
        // centimetre, triangles under twice its 5 cm shortest side read the design's own
        // gradient, and where a face is lower than its rounding (this path's low end) its eased
        // drop between the outline and the toe line, a few centimetres apart, is steeper than
        // the grade: a design property recorded on #149, not a sliver.
        const auto side = [] (edit::Vec3 p, edit::Vec3 q) { return std::hypot (q.x - p.x, q.y - p.y); };
        const double ab = side (a, b), bc = side (b, c), ca = side (c, a);
        const double longest = std::max ({ ab, bc, ca });
        const double middle = std::max ({ std::min (ab, bc), std::min (bc, ca), std::min (ca, ab) });
        const double smallest = std::asin (std::min (1.0, std::abs (u.x * v.y - u.y * v.x) / (longest * middle)));
        slivers += slope > 0.8 && smallest < 10.0 * 3.14159265358979 / 180 && longest >= 0.1;
    }
    EXPECT_EQ (slivers, 0u);
}

// #149 T1 acceptance (the mandatory fixture): two joining paths with different section grades and
// an outline request, built before and after a graded region is added away from them. Both paths
// keep their outline grading -- every face line vertex has the same height -- the region grades its
// own domain, and nothing is said about a band or about paths left out.
TEST (EditTerrainDesignJob, TwoPathsKeepTheirOutlineGradingWhenARegionIsAddedAwayFromThem149)
{
    DesignFixture fixture;
    std::string error;
    // Path B joins A at its middle and runs north, with its own steeper faces.
    const Id b = edit::AddPath (fixture.doc, { { 20, 0, 11 }, { 20, 20, 13.5 } }, 3.0);
    ASSERT_TRUE (edit::EditPath (
        fixture.doc, b,
        [] (edit::Path& value, std::string&) {
            for (auto& section : value.sections) {
                section.crossfall = edit::PercentCrossfall (0);
                section.cutSlope = { TerrainSlope::Unit::Percent, 100, 0 }; // 1:1
                section.fillSlope = { TerrainSlope::Unit::Percent, 100, 0 };
                section.rounding = 0;
            }
            return true;
        },
        error))
        << error;
    LandscapeSource centreline;
    centreline.pathId = b;
    centreline.role = SourceRole::PathCenterline;
    Id sourceB = 0, strokeB = 0;
    ASSERT_TRUE (AddLandscapeSource (fixture.doc, centreline, sourceB, error)) << error;
    LandscapeOperation stroke;
    stroke.kind = OperationKind::Stroke;
    stroke.inputs = { sourceB };
    stroke.width = 3;
    ASSERT_TRUE (AddLandscapeOperation (fixture.doc, stroke, strokeB, error)) << error;
    // An authored property line inside the terrain: the region build's coverage ring names it.
    LandscapeSource property;
    property.entityId =
        edit::AddPolyline (fixture.doc, { { -20, -30, 0 }, { 59, -30, 0 }, { 59, 29, 0 }, { -20, 29, 0 } }, true);
    Id propertyId = 0;
    ASSERT_TRUE (AddLandscapeSource (fixture.doc, property, propertyId, error)) << error;
    ASSERT_TRUE (SetLandscapePropertyLine (fixture.doc, propertyId, error)) << error;
    // As a production capture does, its catalog names every operation of the document.
    const auto capture = [&] () {
        fixture.Capture ();
        for (const auto& op : fixture.doc.landscape->operations)
            if (std::none_of (fixture.provenance.table.operations.begin (), fixture.provenance.table.operations.end (),
                              [&] (const auto& row) { return row.id == op.id; }))
                fixture.provenance.table.operations.push_back ({ op.id, {} });
    };
    capture ();
    auto options = fixture.Options ();
    options.outline = true;
    const auto run = [&] () {
        TerrainDesignPublisher publisher;
        TerrainDesignJob job;
        EXPECT_TRUE (publisher.Prepare (fixture.doc, Capture (fixture), options, {}, job, error)) << error;
        return RunLandscapeTerrainDesign (job);
    };
    const auto before = run ();
    ASSERT_TRUE (before.built) << before.error;
    ASSERT_TRUE (before.published);

    // A graded region well away from both paths' faces.
    const auto line =
        edit::AddPolyline (fixture.doc, { { -15, -25, 0 }, { -5, -25, 0 }, { -5, -15, 0 }, { -15, -15, 0 } }, true);
    ASSERT_TRUE (SetLandscapeSourceRole (fixture.doc, line, SourceRole::RegionBoundary, error)) << error;
    const Id region = fixture.doc.landscape->objects.back ().id;
    RegionSurface surface;
    surface.elevation = RegionElevationMode::Constant;
    surface.first.z = 8;
    ASSERT_TRUE (SetRegionSurface (fixture.doc, region, surface, {}, error)) << error;
    RegionTerrain terrain;
    terrain.effect = RegionTerrainEffect::Grade;
    ASSERT_TRUE (SetRegionTerrain (fixture.doc, region, terrain, error)) << error;
    capture ();
    const auto after = run ();
    ASSERT_TRUE (after.built) << after.error;
    ASSERT_TRUE (after.published);

    for (const auto* outcome : { &before, &after })
        for (const auto& note : outcome->unmet) {
            EXPECT_EQ (note.find ("band"), std::string::npos) << note;
            EXPECT_EQ (note.find ("not graded"), std::string::npos) << note;
        }
    // Every vertex both builds share: the same height with and without the region. The outline's
    // faces get their vertices from the kernel's mesher (#149 T3), so the shared ones are the
    // outline's own, the toe and creases traced from its faces, and the mesher's where the
    // region did not change its order of work.
    const auto height = [] (const TerrainDesignOutcome& outcome, double x, double y) -> std::optional<double> {
        const auto& p = outcome.result.build.points;
        for (size_t i = 0; i + 2 < p.size (); i += 3)
            if (p[i] == x && p[i + 1] == y)
                return p[i + 2];
        return std::nullopt;
    };
    std::map<std::pair<double, double>, double> afterHeights;
    {
        const auto& p = after.result.build.points;
        for (size_t i = 0; i + 2 < p.size (); i += 3)
            afterHeights.emplace (std::pair { p[i], p[i + 1] }, p[i + 2]);
    }
    size_t compared = 0, nearB = 0;
    const auto& points = before.result.build.points;
    for (size_t i = 0; i + 2 < points.size (); i += 3) {
        const double x = points[i], y = points[i + 1];
        const auto found = afterHeights.find ({ x, y });
        // Away from the region's domain and its margin, where the region changes the ground.
        if (found == afterHeights.end () || (x < 0 && y < -10))
            continue;
        ++compared;
        EXPECT_NEAR (points[i + 2], found->second, 1e-9) << x << ", " << y;
        const double ground = 10 + .05 * x - .2 * y;
        nearB += std::abs (x - 20) < 4 && y > 8 && std::abs (found->second - ground) > 0.05;
    }
    EXPECT_GT (compared, 200u);
    EXPECT_GT (nearB, 10u) << "path B is graded from its own outline too";
    // The region grades its own domain to its level.
    const auto inside = height (after, -15, -25);
    ASSERT_TRUE (inside.has_value ());
    EXPECT_NEAR (*inside, 8, 1e-9);
}

// #149: a fixed line and a fixed point at the line's own vertex, with the band or the outline.
// The unified build used to look a point control's spot up by its control index, so a line control
// before it ran past the end and refused as "RegionTopology"; and a unified refusal returned
// before its conditions and located conflicts were filled in. Both requests now lock both controls,
// and raising the line refuses with the conflicts located on it.
TEST (HudTerrainDesign, ALineAndItsOwnVertexFixedBuildOrRefuseLocatedForBandAndOutline149)
{
    for (const bool band : { true, false })
        for (const double z : { 10.0, 11.0 }) {
            SCOPED_TRACE (std::string (band ? "band " : "outline ") + std::to_string (z));
            TerrainHostFixture fixture;
            LandscapeSource guide;
            guide.entityId = edit::AddPolyline (fixture.doc, { { 10, -1, z }, { 20, 1, z } }, false);
            Id source = 0;
            std::string error;
            ASSERT_TRUE (AddLandscapeSource (fixture.doc, guide, source, error)) << error;
            const auto vertex = edit::Find (fixture.doc, guide.entityId)->vertices.front ().id;
            ASSERT_TRUE (SetLandscapeTerrainControl (fixture.doc, source, 0, true, error)) << error;
            ASSERT_TRUE (SetLandscapeTerrainControl (fixture.doc, source, vertex, true, error)) << error;
            eh::Session session (fixture.doc);
            session.terrainAutoBuild = true; // the mesh build lane (#175: the editor previews)
            session.terrainBand = band;
            ASSERT_TRUE (eh::StartTerrainDesign (session, fixture.mesh, fixture.path)) << session.message;
            layers::Layer layer;
            const bool raised = z > 10;
            ASSERT_TRUE (AwaitTerrain (session, layer, raised));
            const auto* current = eh::CurrentTerrainDesign (session);
            ASSERT_NE (current, nullptr);
            if (!raised) {
                EXPECT_EQ (current->error, "");
                EXPECT_GE (current->result.locked.size (), 2u) << "the line's vertices and the point";
                continue;
            }
            EXPECT_EQ (current->error.rfind ("BuildLockedConflict", 0), 0u) << current->error;
            ASSERT_FALSE (current->unmet.empty ()) << "the refusal says what was not met";
            ASSERT_FALSE (current->conflicts.empty ()) << "and where";
            EXPECT_EQ (current->conflicts.front ().code, HybridConflictCode::LockedConflict);
            ASSERT_TRUE (current->conflicts.front ().control);
            EXPECT_EQ (current->conflicts.front ().control->source, source);
        }
}
