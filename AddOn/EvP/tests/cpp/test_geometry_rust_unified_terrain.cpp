#include "Geometry/GeometryRustUnifiedTerrain.hpp"

#include "GeometryRustFfi.h"

#include "Edit/EditPath.hpp"
#include "Edit/EditLandscapeUnifiedBuild.hpp"
#include "virtual_editor_debug_replay.hpp"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace tapioca::geometry;
namespace lg = tapioca::geometry;
namespace edit = geomsrv::edit;

// 9 x 9 vertices at 1 m on z = 0 over [0, 8] x [0, 8]; rings land on mesh edges.
struct Base {
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    Base ()
    {
        for (uint32_t row = 0; row < 9; ++row)
            for (uint32_t column = 0; column < 9; ++column)
                points.insert (points.end (), { double (column), double (row), 0.0 });
        for (uint32_t row = 0; row < 8; ++row)
            for (uint32_t column = 0; column < 8; ++column) {
                const uint32_t a = row * 9 + column;
                triangles.insert (triangles.end (), { a, a + 1, a + 10, a, a + 10, a + 9 });
            }
    }
};

const std::array<double, 12> kProperty { 0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0 };
const std::array<double, 12> kDomain { 2, 2, 0, 6, 2, 0, 6, 6, 0, 2, 6, 0 };

ProvenanceRequest Provenance (EntityCounts counts, uint32_t domains)
{
    ProvenanceRequest request;
    request.expectedSession = { 1 };
    request.counts = counts;
    request.domains = domains;
    auto& table = request.table;
    table.context = { 1, request.expectedSession, 0, 100 };
    table.sources = { { 0x20000000000001ULL, {} } };
    table.operations = { { 12, {} } };
    table.objects = { { 13, 12 } };
    table.sourceIds = { 0x20000000000001ULL };
    table.operationIds = { 12 };
    for (auto [domain, count] : { std::pair { IdentityDomain::Vertex, counts.vertices },
                                  { IdentityDomain::Edge, counts.edges },
                                  { IdentityDomain::Face, counts.faces } }) {
        for (uint64_t index = 0; index < count; ++index) {
            EntityMapping row;
            row.domain = domain;
            row.kind = MappingKind::Generated;
            row.outputIndex = index;
            row.sources = { 0, 1 };
            row.operations = { 0, 1 };
            row.object = 13;
            table.mappings.push_back (row);
        }
    }
    return request;
}

// Unified provenance: mesh vertices, property ring, one four-vertex entry ring.
EntityCounts UnifiedCounts (const Base& base)
{
    return { uint64_t (base.points.size () / 3 + kProperty.size () / 3 + kDomain.size () / 3),
             uint64_t (kProperty.size () / 3 + kDomain.size () / 3), uint64_t (base.triangles.size () / 3), 0 };
}

UnifiedEntry GradeEntry (double level, UnifiedTransitionKind transition, double distance)
{
    UnifiedEntry entry;
    entry.order = 0;
    entry.object = 0x101;
    entry.rootOperation = 0x202;
    static const std::array<std::span<const double>, 0> kNoHoles {};
    static const std::array kParts { UnifiedDomainPart { std::span<const double> (kDomain), kNoHoles } };
    static const std::array<uint64_t, 1> kSources { 0x20000000000001ULL };
    entry.parts = std::span<const UnifiedDomainPart> (kParts);
    entry.sources = std::span<const uint64_t> (kSources);
    entry.surface = { UnifiedSurfaceKind::Constant, level };
    entry.effect = UnifiedTerrainEffect::Grade;
    entry.transition = { transition, distance, {}, {}, 0 };
    if (transition == UnifiedTransitionKind::Daylight) {
        entry.transition.cut = { TerrainSlope::Unit::Percent, 100, 0 };
        entry.transition.fill = { TerrainSlope::Unit::Percent, 100, 0 };
        entry.transition.reach = distance;
    }
    return entry;
}

TerrainStation Station (double x, double z)
{
    TerrainStation station;
    station.position = { x, 4, z };
    station.halfLeft = station.halfRight = 1;
    station.cut = station.fill = { TerrainSlope::Unit::Percent, 100, 0 };
    station.cutLimit = station.fillLimit = 10;
    return station;
}

FieldPlan CompiledPlan (const debugreplay::Scene& scene, const TerrainCapture& capture, bool includePath)
{
    LandscapeSnapshot snapshot;
    snapshot.doc = &scene.doc;
    snapshot.revision = scene.doc.revision;
    snapshot.captureRevision = capture.revision;
    snapshot.session = scene.doc.landscape->sessionUuid;
    snapshot.sourceTerrainGuid = scene.doc.landscape->sourceTerrainGuid;
    if (includePath) {
        PathDesignRequest path;
        path.source = scene.options.source;
        path.operation = scene.options.operation;
        path.object = scene.options.object;
        path.maxStep = scene.options.maxStep;
        path.taper = scene.options.taper;
        path.margin = scene.options.margin;
        path.arcTolerance = scene.options.arcTolerance;
        path.cutLimit = scene.options.cutLimit;
        path.fillLimit = scene.options.fillLimit;
        path.effect = scene.options.effect;
        path.tolerance = scene.options.tolerance;
        path.outline = scene.options.outline;
        snapshot.path = path;
    }
    return CompileLandscapeFieldPlan (snapshot);
}

debugreplay::Scene Issue82Scene ()
{
    const auto file = std::filesystem::path (__FILE__).parent_path () / "fixtures/region-boundary-20261029.debug.json";
    std::ifstream input (file, std::ios::binary);
    if (!input)
        throw std::runtime_error ("missing original #82 fixture");
    const std::string text { std::istreambuf_iterator<char> (input), {} };
    const auto parsed = evp::nodegraph::json::Parse (text);
    if (!parsed.ok)
        throw std::runtime_error (parsed.error);
    auto scene = debugreplay::Read (parsed.value);
    // The 2026-10-29 capture asked for outline grading, which the mixed lane graded as the
    // selected path's band until #149 T1. These tests pin that band contract, so they ask for
    // the band; the outline over the same capture has its own tests.
    scene.options.outline = false;
    return scene;
}

} // namespace

TEST (GeometryRustUnifiedTerrain, AbiLayouts)
{
    static_assert (sizeof (TapiocaUnifiedPart) == 8);
    static_assert (sizeof (TapiocaUnifiedEntry) == 136);
    static_assert (sizeof (TapiocaUnifiedPath) == 56);
    static_assert (sizeof (TapiocaUnifiedMask) == 32);
    static_assert (sizeof (TapiocaUnifiedControl) == 56);
    static_assert (sizeof (TapiocaUnifiedCounts) == 40);
    static_assert (sizeof (TapiocaUnifiedMeta) == 24);
    static_assert (sizeof (TapiocaUnifiedOutline) == 120);
    static_assert (sizeof (TapiocaUnifiedLine) == 32);
    static_assert (sizeof (TapiocaUnifiedLocked) == 16);
    static_assert (sizeof (void*) != 8 || sizeof (TapiocaUnifiedRequest) == 576);
    static_assert (sizeof (void*) != 8 || sizeof (TapiocaUnifiedOutput) == 152);
    SUCCEED ();
}

TEST (GeometryRustUnifiedTerrain, RegionGradeMatchesTheExistingEditLane)
{
    const Base base;
    const auto entries = { GradeEntry (2.0, UnifiedTransitionKind::Falloff, 1.0) };
    const UnifiedPlanRequest plan {
        { base.points, base.triangles },     std::span<const double> (kProperty), entries, {}, {}, {}, 1e-3, 7, 0xABCD,
        Provenance (UnifiedCounts (base), 7)
    };
    UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (BuildUnifiedTerrain (plan, unified, error)) << error;
    EXPECT_EQ (unified.revision, 7U);
    EXPECT_EQ (unified.planHash, 0xABCD);
    // Same field through the existing S3b.3 edit lane: one absolute area edit
    // with a 1 m falloff must move every input vertex to the same height.
    TerrainEdit edit;
    edit.target = TerrainEditTarget::Area;
    edit.geometry = kDomain;
    edit.level = 2.0;
    edit.mode = TerrainEditMode::Absolute;
    edit.effect = TerrainEditEffect::Both;
    edit.transition = TerrainEditTransition::Falloff;
    edit.blur = 1.0;
    const TerrainEditRequest editRequest {
        { base.points, base.triangles },
        { &edit, 1 },
        {},
        {},
        {},
        std::nullopt,
        Provenance ({ uint64_t (base.points.size () / 3), 0, uint64_t (base.triangles.size () / 3), 0 }, 5)
    };
    TerrainEditResult edited;
    ASSERT_TRUE (EditTerrain (editRequest, edited, error)) << error;
    ASSERT_EQ (edited.heights.size (), base.points.size () / 3);
    for (size_t vertex = 0; vertex < edited.heights.size (); ++vertex) {
        ASSERT_LT (vertex * 3 + 2, unified.points.size ());
        EXPECT_NEAR (unified.points[vertex * 3 + 2], edited.heights[vertex], 1e-9)
            << "vertex " << vertex << " x=" << base.points[vertex * 3];
    }
}

TEST (GeometryRustUnifiedTerrain, PathBandMatchesTheExistingHybridLane)
{
    const Base base;
    const std::array stations { Station (2, 1), Station (6, 1) };
    const UnifiedPath path {
        0, 0x3001, 0x3002, 0x3003, std::span<const TerrainStation> (stations), TerrainEditEffect::Both, 1e-3
    };
    const std::array paths { path };
    const UnifiedPlanRequest plan { { base.points, base.triangles },
                                    std::span<const double> (kProperty),
                                    {},
                                    std::span<const UnifiedPath> (paths),
                                    {},
                                    {},
                                    1e-3,
                                    3,
                                    0xBEEF,
                                    Provenance ({ uint64_t (base.points.size () / 3 + kProperty.size () / 3),
                                                  uint64_t (kProperty.size () / 3),
                                                  uint64_t (base.triangles.size () / 3), 0 },
                                                7) };
    UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (BuildUnifiedTerrain (plan, unified, error)) << error;
    // The same band on the same base inside the same property ring, through the
    // S4.2 hybrid lane: identical constraints must give the same surface.
    const HybridTerrainRequest band { { base.points, base.triangles },
                                      std::span<const double> (kProperty),
                                      std::span<const TerrainStation> (stations),
                                      TerrainEditEffect::Both,
                                      1e-3,
                                      plan.provenance,
                                      {},
                                      {} };
    HybridTerrainResult hybrid;
    ASSERT_TRUE (BuildHybridTerrain (band, hybrid, error)) << error;
    ASSERT_EQ (unified.points.size (), hybrid.points.size ());
    ASSERT_EQ (unified.triangles.size (), hybrid.triangles.size ());
    for (size_t at = 0; at + 2 < unified.points.size (); at += 3) {
        bool found = false;
        double hybridHeight = 0;
        for (size_t bt = 0; bt + 2 < hybrid.points.size (); bt += 3) {
            if (std::abs (hybrid.points[bt] - unified.points[at]) < 1e-9 &&
                std::abs (hybrid.points[bt + 1] - unified.points[at + 1]) < 1e-9) {
                found = true;
                hybridHeight = hybrid.points[bt + 2];
                break;
            }
        }
        ASSERT_TRUE (found) << "vertex at " << unified.points[at] << ", " << unified.points[at + 1];
        EXPECT_NEAR (hybridHeight, unified.points[at + 2], 1e-9);
    }
    // The band contributes derived design-line origins after the property ring.
    ASSERT_FALSE (unified.lines.empty ());
    EXPECT_TRUE (std::any_of (unified.lines.begin (), unified.lines.end (), [] (const UnifiedLine& line) {
        return line.domain == UnifiedLineDomain::Path && line.kind && *line.kind == HybridLineKind::Edge;
    }));
}

TEST (GeometryRustUnifiedTerrain, AdjacentSharpGradesRefuseWithTheHardStepCode)
{
    const Base base;
    auto left = GradeEntry (1.0, UnifiedTransitionKind::Daylight, 0.0);
    auto right = GradeEntry (3.0, UnifiedTransitionKind::Daylight, 0.0);
    const std::array<double, 12> kLeft { 2, 2, 0, 4, 2, 0, 4, 6, 0, 2, 6, 0 };
    const std::array<double, 12> kRight { 4, 2, 0, 6, 2, 0, 6, 6, 0, 4, 6, 0 };
    static const std::array<std::span<const double>, 0> kNoHoles {};
    const std::array kLeftParts { UnifiedDomainPart { std::span<const double> (kLeft), kNoHoles } };
    const std::array kRightParts { UnifiedDomainPart { std::span<const double> (kRight), kNoHoles } };
    left.parts = std::span<const UnifiedDomainPart> (kLeftParts);
    right.parts = std::span<const UnifiedDomainPart> (kRightParts);
    right.order = 1;
    const std::array entries { left, right };
    const UnifiedPlanRequest plan { { base.points, base.triangles },
                                    std::span<const double> (kProperty),
                                    std::span<const UnifiedEntry> (entries),
                                    {},
                                    {},
                                    {},
                                    1e-3,
                                    9,
                                    0xFEED,
                                    Provenance ({ uint64_t (base.points.size () / 3 + kProperty.size () / 3 + 8),
                                                  uint64_t (kProperty.size () / 3 + 8),
                                                  uint64_t (base.triangles.size () / 3), 0 },
                                                7) };
    UnifiedTerrainResult result;
    std::string error;
    const UnifiedTerrainResult before;
    EXPECT_FALSE (BuildUnifiedTerrain (plan, result, error));
    EXPECT_EQ (0U, error.rfind ("BuildHardStep", 0)) << error;
    EXPECT_EQ (result, before);
}

TEST (GeometryRustUnifiedTerrain, FixedPointControlLocksItsVertex)
{
    const Base base;
    const auto entries = { GradeEntry (2.0, UnifiedTransitionKind::Daylight, 1.0) };
    const UnifiedControl control { 0x4001, 0x4002, true, { 4, 4, 2.0 }, {} };
    const std::array controls { control };
    auto counts = UnifiedCounts (base);
    counts.vertices += 1; // One point control adds one provenance vertex.
    const UnifiedPlanRequest plan { { base.points, base.triangles },
                                    std::span<const double> (kProperty),
                                    entries,
                                    {},
                                    {},
                                    std::span<const UnifiedControl> (controls),
                                    1e-3,
                                    11,
                                    0xF00D,
                                    Provenance (counts, 7) };
    UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (BuildUnifiedTerrain (plan, unified, error)) << error;
    ASSERT_EQ (unified.locked.size (), 1U);
    EXPECT_EQ (unified.locked[0].control, 0U);
    ASSERT_LT (size_t (unified.locked[0].vertex) * 3 + 2, unified.points.size ());
    EXPECT_NEAR (unified.points[size_t (unified.locked[0].vertex) * 3 + 2], 2.0, 1e-12);
    EXPECT_TRUE (std::any_of (unified.mappings.begin (), unified.mappings.end (), [] (const TerrainMapping& row) {
        return row.kind == TerrainMappingKind::ControlPointVertex;
    }));
    EXPECT_EQ (unified.inputProvenance, plan.provenance);
}

TEST (GeometryRustUnifiedTerrain, TwoPointFixedLineBuildsWithSingleCatalogCoverage)
{
    const Base base;
    const auto entries = { GradeEntry (1.0, UnifiedTransitionKind::Daylight, 1.0) };
    const std::array<double, 6> line { 2, 4, 1, 6, 4, 1 };
    const UnifiedControl control { 0x4001, 0, false, {}, line };
    const std::array controls { control };
    auto counts = UnifiedCounts (base);
    counts.vertices += 2;
    counts.edges += 1;
    const UnifiedPlanRequest plan { { base.points, base.triangles },
                                    std::span<const double> (kProperty),
                                    entries,
                                    {},
                                    {},
                                    std::span<const UnifiedControl> (controls),
                                    1e-3,
                                    12,
                                    0xF11E,
                                    Provenance (counts, 7) };
    UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (BuildUnifiedTerrain (plan, unified, error)) << error;
    ASSERT_GE (unified.locked.size (), 2U);
    for (const auto& locked : unified.locked) {
        EXPECT_EQ (locked.control, 0U);
        ASSERT_LT (size_t (locked.vertex) * 3 + 2, unified.points.size ());
        EXPECT_DOUBLE_EQ (unified.points[size_t (locked.vertex) * 3 + 2], 1.0);
    }
    EXPECT_TRUE (std::any_of (unified.lines.begin (), unified.lines.end (), [] (const UnifiedLine& origin) {
        return origin.domain == UnifiedLineDomain::Control && origin.index == 0 && origin.segment == 1;
    }));
}

TEST (GeometryRustUnifiedTerrain, TriangulatedSurfaceTargetSamplesItsPlane)
{
    const Base base;
    auto entry = GradeEntry (0.0, UnifiedTransitionKind::Daylight, 1.0);
    const std::array<double, 12> target { 2, 2, 2.4, 6, 2, 3.2, 6, 6, 3.2, 2, 6, 2.4 };
    const std::array<uint32_t, 6> triangles { 0, 1, 2, 0, 2, 3 };
    entry.surface = { UnifiedSurfaceKind::Triangulated, 0.0, target, triangles };
    const std::array entries { entry };
    const UnifiedPlanRequest plan {
        { base.points, base.triangles },     std::span<const double> (kProperty), entries, {}, {}, {}, 1e-3, 13, 0xF13D,
        Provenance (UnifiedCounts (base), 7)
    };
    UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (BuildUnifiedTerrain (plan, unified, error)) << error;
    const auto height = [&] (double x, double y) {
        for (size_t at = 0; at + 2 < unified.points.size (); at += 3)
            if (unified.points[at] == x && unified.points[at + 1] == y)
                return unified.points[at + 2];
        return std::numeric_limits<double>::quiet_NaN ();
    };
    EXPECT_DOUBLE_EQ (height (4, 4), 2.8);
    EXPECT_DOUBLE_EQ (height (2, 2), 2.4);
}

TEST (GeometryRustUnifiedTerrain, EmptyPlanWithoutPropertyIsBaselineBitForBit)
{
    const Base base;
    const UnifiedPlanRequest plan {
        { base.points, base.triangles },
        {},
        {},
        {},
        {},
        {},
        1e-3,
        0,
        0,
        Provenance ({ uint64_t (base.points.size () / 3), 0, uint64_t (base.triangles.size () / 3), 0 }, 7)
    };
    UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (BuildUnifiedTerrain (plan, unified, error)) << error;
    EXPECT_EQ (unified.points, base.points);
    EXPECT_EQ (unified.triangles, base.triangles);
    EXPECT_TRUE (std::all_of (unified.owners.begin (), unified.owners.end (),
                              [] (const std::optional<uint32_t>& owner) { return !owner; }));
    EXPECT_EQ (unified.site.fill, 0.0);
    EXPECT_EQ (unified.site.cut, 0.0);
}

TEST (GeometryRustUnifiedTerrain, NestedSharpOverridesRefuseInsteadOfInterpolating)
{
    const Base base;
    auto outer = GradeEntry (3.0, UnifiedTransitionKind::Daylight, 0.0);
    auto inner = GradeEntry (1.0, UnifiedTransitionKind::Daylight, 0.0);
    const std::array<double, 12> kOuter { 2, 2, 0, 7, 2, 0, 7, 7, 0, 2, 7, 0 };
    const std::array<double, 12> kInner { 4, 4, 0, 6, 4, 0, 6, 6, 0, 4, 6, 0 };
    static const std::array<std::span<const double>, 0> kNoHoles {};
    const std::array kOuterParts { UnifiedDomainPart { std::span<const double> (kOuter), kNoHoles } };
    const std::array kInnerParts { UnifiedDomainPart { std::span<const double> (kInner), kNoHoles } };
    outer.parts = std::span<const UnifiedDomainPart> (kOuterParts);
    inner.parts = std::span<const UnifiedDomainPart> (kInnerParts);
    inner.order = 1;
    const std::array entries { outer, inner };
    const UnifiedPlanRequest plan { { base.points, base.triangles },
                                    std::span<const double> (kProperty),
                                    std::span<const UnifiedEntry> (entries),
                                    {},
                                    {},
                                    {},
                                    1e-3,
                                    5,
                                    0x5EED,
                                    Provenance ({ uint64_t (base.points.size () / 3 + kProperty.size () / 3 + 8),
                                                  uint64_t (kProperty.size () / 3 + 8),
                                                  uint64_t (base.triangles.size () / 3), 0 },
                                                7) };
    UnifiedTerrainResult unified;
    const UnifiedTerrainResult before;
    std::string error;
    EXPECT_FALSE (BuildUnifiedTerrain (plan, unified, error));
    EXPECT_EQ (0U, error.rfind ("BuildHardStep", 0)) << error;
    EXPECT_EQ (unified, before);
}

TEST (GeometryRustUnifiedTerrain, CompiledPlanBuildsTheOriginalIssue82Scene)
{
    auto scene = Issue82Scene ();
    scene.options.source = 0;
    const auto regionOnly = debugreplay::Run (scene);
    ASSERT_TRUE (regionOnly.built) << regionOnly.error;
    scene.options.source = 8;
    const auto plan = CompiledPlan (scene, *regionOnly.capture, true);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_FALSE (plan.entries.empty ());
    ASSERT_EQ (plan.paths.size (), 1u);

    UnifiedTerrainResult unified;
    UnifiedLandscapeBuildScope scope;
    std::string error;
    ASSERT_TRUE (BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, unified, scope, error))
        << error;
    EXPECT_EQ (unified.revision, plan.revision);
    EXPECT_EQ (unified.planHash, plan.hash);
    EXPECT_GT (unified.inputProvenance.counts.vertices, regionOnly.capture->provenance.counts.vertices);
    EXPECT_EQ (unified.inputProvenance.domains, 7u);
    EXPECT_TRUE (unified.locked.empty ());

    const std::array<double, 6> query { 6.5, 6, 10.5921960923966, 14.2999855870725, 14.2999855870725, 0.5 };
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    ASSERT_TRUE (SampleTerrain ({ unified.points, unified.triangles }, query, 0.25, samples, summary, error)) << error;
    ASSERT_EQ (samples.size (), 3u);
    EXPECT_NEAR (samples[0].height, 0, 1e-6);
    EXPECT_NEAR (samples[1].height, 0.07, 1e-6);
    EXPECT_NEAR (samples[2].height, -0.2, 1e-12);
}

TEST (GeometryRustUnifiedTerrain, CompiledPlanBuildsATwoPointSurface)
{
    auto scene = Issue82Scene ();
    scene.options.source = 0;
    const auto regionIds = TerrainRegionObjects (scene.doc);
    ASSERT_EQ (regionIds.size (), 1u);
    auto& object = *std::find_if (scene.doc.landscape->objects.begin (), scene.doc.landscape->objects.end (),
                                  [&] (const auto& candidate) { return candidate.id == regionIds.front (); });
    object.surface->elevation = RegionElevationMode::Constant;
    object.surface->slope = RegionSlopeMode::TwoPoints;
    object.surface->first = { 0, 0, 0 };
    object.surface->second = { 10, 0, 2 };
    const auto regionOnly = debugreplay::Run (scene);
    ASSERT_TRUE (regionOnly.built) << regionOnly.error;
    const auto plan = CompiledPlan (scene, *regionOnly.capture, false);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_EQ (plan.entries.size (), 1u);
    EXPECT_FALSE (plan.entries.front ().surfacePoints.empty ());
    EXPECT_FALSE (plan.entries.front ().surfaceTriangles.empty ());

    UnifiedTerrainResult unified;
    UnifiedLandscapeBuildScope scope;
    std::string error;
    ASSERT_TRUE (BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, unified, scope, error))
        << error;
    const std::array<double, 2> query { 6.5, 6 };
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    ASSERT_TRUE (SampleTerrain ({ unified.points, unified.triangles }, query, 0.25, samples, summary, error)) << error;
    ASSERT_EQ (samples.size (), 1u);
    EXPECT_NEAR (samples.front ().height, 1.3, 1e-9);
}

TEST (GeometryRustUnifiedTerrain, CompiledPlanBuildsContourAndBoundaryTargets)
{
    for (const bool boundary : { false, true }) {
        auto scene = Issue82Scene ();
        scene.options.source = 0;
        const auto regionIds = TerrainRegionObjects (scene.doc);
        ASSERT_EQ (regionIds.size (), 1u);
        auto& object = *std::find_if (scene.doc.landscape->objects.begin (), scene.doc.landscape->objects.end (),
                                      [&] (const auto& candidate) { return candidate.id == regionIds.front (); });
        if (boundary) {
            object.surface->elevation = RegionElevationMode::Boundary;
            object.surface->slope = RegionSlopeMode::None;
        }
        else {
            object.surface->elevation = RegionElevationMode::Constant;
            object.surface->slope = RegionSlopeMode::ContourLine;
            object.surface->first = { 0, 0, 2 };
            object.surface->second = { 10, 0, 2 };
            object.surface->grade = { TerrainSlope::Unit::Percent, 20, 0 };
            object.surface->fallsTo = RegionFallSide::Right;
        }
        const auto regionOnly = debugreplay::Run (scene);
        ASSERT_TRUE (regionOnly.built) << regionOnly.error;
        const auto plan = CompiledPlan (scene, *regionOnly.capture, false);
        ASSERT_TRUE (plan.ok) << plan.error;
        ASSERT_EQ (plan.entries.size (), 1u);
        EXPECT_FALSE (plan.entries.front ().surfacePoints.empty ());
        EXPECT_FALSE (plan.entries.front ().surfaceTriangles.empty ());

        UnifiedTerrainResult unified;
        UnifiedLandscapeBuildScope scope;
        std::string error;
        ASSERT_TRUE (BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, unified, scope, error))
            << error;
        const std::array<double, 4> query { 6.5, 6, 5.0, 5.0 };
        std::vector<TerrainSample> actual, expected;
        TerrainFieldSummary actualSummary, expectedSummary;
        ASSERT_TRUE (SampleTerrain ({ unified.points, unified.triangles }, query, 0.25, actual, actualSummary, error))
            << error;
        // Legacy comparison surface: the region evaluation the P4c production
        // lane still runs as its diagnostic stage (identical authority to the
        // removed composition's input).
        const auto& evaluation = regionOnly.regions->evaluation.terrain;
        ASSERT_TRUE (
            SampleTerrain ({ evaluation.points, evaluation.triangles }, query, 0.25, expected, expectedSummary, error))
            << error;
        ASSERT_EQ (actual.size (), 2u);
        ASSERT_EQ (expected.size (), 2u);
        for (size_t index = 0; index < actual.size (); ++index)
            // Legacy target preparation has ~1e-8 m Clipper quantization noise;
            // the original #82 acceptance uses this same 1e-6 comparison band.
            EXPECT_NEAR (actual[index].height, expected[index].height, 1e-6) << "boundary=" << boundary;
        if (!boundary)
            EXPECT_NEAR (actual.front ().height, 3.2, 1e-9);
    }
}

TEST (GeometryRustUnifiedTerrain, IncompleteCompiledPathRefusesWithoutPartialOutput)
{
    auto scene = Issue82Scene ();
    scene.options.source = 0;
    const auto regionOnly = debugreplay::Run (scene);
    ASSERT_TRUE (regionOnly.built) << regionOnly.error;
    LandscapeSnapshot snapshot;
    snapshot.doc = &scene.doc;
    snapshot.revision = scene.doc.revision;
    snapshot.captureRevision = regionOnly.capture->revision;
    snapshot.session = scene.doc.landscape->sessionUuid;
    snapshot.sourceTerrainGuid = scene.doc.landscape->sourceTerrainGuid;
    snapshot.path = PathDesignRequest {};
    snapshot.path->source = scene.doc.nextId + 100;
    const auto plan = CompileLandscapeFieldPlan (snapshot);
    ASSERT_TRUE (plan.ok);
    ASSERT_FALSE (plan.diagnostics.empty ());
    UnifiedTerrainResult result;
    result.points = { 42 };
    const auto before = result;
    UnifiedLandscapeBuildScope scope;
    std::string error;
    EXPECT_FALSE (BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, result, scope, error));
    EXPECT_EQ (0U, error.rfind ("FieldPlanIncomplete:", 0)) << error;
    EXPECT_EQ (result, before);
}

// The original #82 dump: a captured native solid, one region object graded to
// a constant, and one path whose band crosses the region. The production
// orchestrator builds it region-first/path-second; one unified build must
// reach the same heights on the same queries.
TEST (GeometryRustUnifiedTerrain, MixedRegionPathMatchesProductionOnIssue82Fixture)
{
    const auto file = std::filesystem::path (__FILE__).parent_path () / "fixtures/region-boundary-20261029.debug.json";
    std::ifstream input (file, std::ios::binary);
    ASSERT_TRUE (input);
    const std::string text { std::istreambuf_iterator<char> (input), {} };
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    auto scene = debugreplay::Read (parsed.value);
    // The band contract this test pins by hand (the capture asked for an outline, its own stage
    // since #149 T1).
    scene.options.outline = false;

    scene.options.source = 0;
    const auto regionOnly = debugreplay::Run (scene);
    ASSERT_TRUE (regionOnly.built) << regionOnly.error;
    scene.options.source = 8;
    const auto mixed = debugreplay::Run (scene);
    ASSERT_TRUE (mixed.built) << mixed.error;
    ASSERT_FALSE (mixed.result.region.empty ()) << "the path lane derived its reach ring";
    std::string error;

    // The unified baseline is the same prepared surface both lanes ran on.
    const auto& baseline = *regionOnly.capture;
    const auto mesh = TerrainMeshView { baseline.points, baseline.triangles };

    // Region entry translated from the document's region object.
    const auto regionIds = lg::TerrainRegionObjects (scene.doc);
    ASSERT_EQ (regionIds.size (), 1u);
    const auto& object = *std::find_if (scene.doc.landscape->objects.begin (), scene.doc.landscape->objects.end (),
                                        [&] (const auto& candidate) { return candidate.id == regionIds[0]; });
    ASSERT_TRUE (object.surface);
    const auto& settings = object.terrain;
    UnifiedEntry entry;
    entry.order = 0;
    entry.object = object.id;
    entry.rootOperation = object.footprintOperation;
    entry.effect = UnifiedTerrainEffect::Grade;
    entry.surface = { UnifiedSurfaceKind::Constant, object.surface->first.z };
    entry.transition = { settings.transition == lg::RegionTerrainTransition::Daylight ? UnifiedTransitionKind::Daylight
                                                                                      : UnifiedTransitionKind::Falloff,
                         settings.falloff,
                         {},
                         {},
                         settings.reach };
    if (entry.transition.kind == UnifiedTransitionKind::Daylight) {
        entry.transition.cut = settings.cut;
        entry.transition.fill = settings.fill;
    }

    // Evaluated domain rings for the region object, flattened for the facade.
    std::vector<std::vector<double>> ringBuffers;
    std::vector<UnifiedDomainPart> domainParts;
    std::vector<std::span<const double>> holeSpans;
    for (const auto& region : regionOnly.regions->evaluation.landscape.regions) {
        if (region.objectId != object.id)
            continue;
        for (const auto& ring : region.rings) {
            auto& flat = ringBuffers.emplace_back ();
            for (const auto& point : ring)
                flat.insert (flat.end (), { point.x, point.y, point.z });
        }
    }
    ASSERT_FALSE (ringBuffers.empty ());
    domainParts.push_back (
        { std::span<const double> (ringBuffers.front ()), std::span<const std::span<const double>> (holeSpans) });
    static const std::array<uint64_t, 1> kDomainSources { 0x20000000000001ULL };
    entry.parts = std::span<const UnifiedDomainPart> (domainParts);
    entry.sources = std::span<const uint64_t> (kDomainSources);
    const std::array entries { entry };

    // Path band stations through the same public path API the path lane uses.
    const auto pathSource =
        std::find_if (scene.doc.landscape->sources.begin (), scene.doc.landscape->sources.end (),
                      [&] (const auto& source) { return source.id == scene.options.source && !source.deleted; });
    ASSERT_NE (pathSource, scene.doc.landscape->sources.end ());
    const auto* path = edit::FindPath (scene.doc, pathSource->pathId);
    ASSERT_NE (path, nullptr);
    edit::AlignmentGeometry geometry;
    edit::PathPreparation preparation;
    ASSERT_TRUE (edit::PreparePath (*path, geometry, preparation, error));
    auto samples = edit::SampleStations (*path, geometry, scene.options.maxStep);
    std::sort (samples.begin (), samples.end ());
    samples.erase (std::unique (samples.begin (), samples.end (), [] (double a, double b) { return b - a < 1.0e-6; }),
                   samples.end ());
    std::vector<TerrainStation> stations;
    for (const double station : samples) {
        edit::StationFrame frame;
        ASSERT_TRUE (edit::Evaluate (*path, geometry, preparation, station, frame));
        const auto& section = frame.section;
        double percent = 0;
        ASSERT_TRUE (edit::PreparedCrossfall (preparation, section, percent));
        TerrainStation value;
        value.position = { frame.centre.x, frame.centre.y, frame.centre.z };
        value.halfLeft = section.widthLeft;
        value.halfRight = section.widthRight;
        if (percent != 0.0 && section.fall == edit::Fall::Crown)
            value.crown = TerrainSlope { TerrainSlope::Unit::Percent, percent, 0 };
        else if (percent != 0.0)
            value.crossfall = TerrainCrossfall {
                { TerrainSlope::Unit::Percent, percent, 0 },
                section.fall == edit::Fall::Left ? TerrainHand::Left : TerrainHand::Right,
            };
        value.shoulderLeft = value.shoulderRight = section.shoulder;
        value.cut = section.cutSlope;
        value.fill = section.fillSlope;
        value.cutLimit = scene.options.cutLimit;
        value.fillLimit = scene.options.fillLimit;
        value.rounding = section.rounding;
        stations.push_back (value);
    }
    ASSERT_GE (stations.size (), 2u);

    const size_t ringVertices = mixed.result.region.size () / 3;
    size_t entryVertices = 0;
    for (const auto& ring : ringBuffers)
        entryVertices += ring.size () / 3;
    const lg::EntityCounts provenanceCounts { uint64_t (mesh.points.size () / 3 + ringVertices + entryVertices),
                                              uint64_t (ringVertices + entryVertices),
                                              uint64_t (mesh.triangles.size () / 3), 0 };
    const auto buildPlan = [&] (bool includePath, UnifiedTerrainResult& result) {
        const std::span<const UnifiedPath> noPaths {};
        const std::array kPaths { UnifiedPath { 1, 8, scene.options.operation, object.id,
                                                std::span<const TerrainStation> (stations), scene.options.effect,
                                                scene.options.tolerance } };
        const UnifiedPlanRequest plan { mesh,
                                        std::span<const double> (mixed.result.region),
                                        std::span<const UnifiedEntry> (entries),
                                        includePath ? std::span<const UnifiedPath> (kPaths) : noPaths,
                                        {},
                                        {},
                                        1e-3,
                                        mixed.revision,
                                        0x82,
                                        Provenance (provenanceCounts, 7) };
        return BuildUnifiedTerrain (plan, result, error);
    };

    // The production oracle acceptance queries. Query 0 sits inside the graded
    // hexagon (production carries ~1e-8 daylight boundary noise there, so its
    // own acceptance uses 1e-6); query 1 is on the path centreline; query 2 is
    // remote baseline (exact).
    const std::array<double, 6> query { 6.5, 6, 10.5921960923966, 14.2999855870725, 14.2999855870725, 0.5 };
    const auto sampleAt = [&] (const std::vector<double>& points, const std::vector<uint32_t>& triangles) {
        std::vector<lg::TerrainSample> found;
        lg::TerrainFieldSummary summary;
        std::string message;
        EXPECT_TRUE (lg::SampleTerrain ({ points, triangles }, query, 0.25, found, summary, message)) << message;
        return found;
    };

    UnifiedTerrainResult mixedUnified;
    ASSERT_TRUE (buildPlan (true, mixedUnified)) << error;
    UnifiedTerrainResult repeat;
    ASSERT_TRUE (buildPlan (true, repeat)) << error;
    EXPECT_EQ (repeat.points, mixedUnified.points);
    EXPECT_EQ (repeat.triangles, mixedUnified.triangles);

    // Mixed stage: one unified build reaches the same heights as the
    // region-first/path-second production orchestration on the acceptance
    // queries.
    const auto unifiedSamples = sampleAt (mixedUnified.points, mixedUnified.triangles);
    const auto productionSamples = sampleAt (mixed.result.build.points, mixed.result.build.triangles);
    ASSERT_EQ (unifiedSamples.size (), 3u);
    ASSERT_EQ (productionSamples.size (), 3u);
    for (const auto& sample : unifiedSamples)
        EXPECT_TRUE (sample.onSurface);
    const std::array tolerances { 1e-6, 1e-6, 1e-12 };
    for (size_t i = 0; i < 3; ++i)
        EXPECT_NEAR (unifiedSamples[i].height, productionSamples[i].height, tolerances[i]) << "query " << i;

    // The documented scene values still hold on the unified surface.
    EXPECT_NEAR (unifiedSamples[0].height, 0, 1e-6) << "the region grades the baseline";
    EXPECT_NEAR (unifiedSamples[1].height, 0.07, 1e-6) << "the path centreline takes the profile";
    EXPECT_NEAR (unifiedSamples[2].height, -0.2, 1e-12) << "remote terrain stays at its captured level";

    EXPECT_GT (mixedUnified.site.fill, 0.0);
    EXPECT_TRUE (std::any_of (mixedUnified.owners.begin (), mixedUnified.owners.end (),
                              [] (const std::optional<uint32_t>& owner) { return owner == 0; }));
    EXPECT_TRUE (std::any_of (mixedUnified.owners.begin (), mixedUnified.owners.end (),
                              [] (const std::optional<uint32_t>& owner) { return owner == 1; }));
    EXPECT_TRUE (std::any_of (mixedUnified.lines.begin (), mixedUnified.lines.end (),
                              [] (const UnifiedLine& line) { return line.domain == UnifiedLineDomain::Path; }));
    EXPECT_EQ (mixedUnified.inputProvenance, Provenance (provenanceCounts, 7));
}
