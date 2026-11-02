#include "Geometry/GeometryRustUnifiedTerrain.hpp"

#include "GeometryRustFfi.h"
#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {
using namespace tapioca::geometry;

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
             uint64_t (kProperty.size () / 3 + kDomain.size () / 3),
             uint64_t (base.triangles.size () / 3), 0 };
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

} // namespace

TEST (GeometryRustUnifiedTerrain, AbiLayouts)
{
    static_assert (sizeof (TapiocaUnifiedPart) == 8);
    static_assert (sizeof (TapiocaUnifiedEntry) == 120);
    static_assert (sizeof (TapiocaUnifiedPath) == 56);
    static_assert (sizeof (TapiocaUnifiedMask) == 32);
    static_assert (sizeof (TapiocaUnifiedControl) == 56);
    static_assert (sizeof (TapiocaUnifiedCounts) == 40);
    static_assert (sizeof (TapiocaUnifiedMeta) == 16);
    static_assert (sizeof (TapiocaUnifiedLine) == 32);
    static_assert (sizeof (TapiocaUnifiedLocked) == 16);
    static_assert (sizeof (void*) != 8 || sizeof (TapiocaUnifiedRequest) == 512);
    static_assert (sizeof (void*) != 8 || sizeof (TapiocaUnifiedOutput) == 152);
    SUCCEED ();
}

TEST (GeometryRustUnifiedTerrain, RegionGradeMatchesTheExistingEditLane)
{
    const Base base;
    const auto entries = { GradeEntry (2.0, UnifiedTransitionKind::Falloff, 1.0) };
    const UnifiedPlanRequest plan { { base.points, base.triangles },
                                    std::span<const double> (kProperty),
                                    entries,
                                    {},
                                    {},
                                    {},
                                    1e-3,
                                    7,
                                    0xABCD,
                                    Provenance (UnifiedCounts (base), 7) };
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
    const TerrainEditRequest editRequest { { base.points, base.triangles },
                                           { &edit, 1 },
                                           {},
                                           {},
                                           {},
                                           std::nullopt,
                                           Provenance ({ uint64_t (base.points.size () / 3), 0,
                                                         uint64_t (base.triangles.size () / 3), 0 },
                                                       5) };
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
    const UnifiedPath path { 0,
                             0x3001,
                             0x3002,
                             0x3003,
                             std::span<const TerrainStation> (stations),
                             TerrainEditEffect::Both,
                             1e-3 };
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
    EXPECT_TRUE (std::any_of (unified.lines.begin (), unified.lines.end (),
                              [] (const UnifiedLine& line) {
                                  return line.domain == UnifiedLineDomain::Path && line.kind &&
                                         *line.kind == HybridLineKind::Edge;
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
    const auto entries = { GradeEntry (2.0, UnifiedTransitionKind::Daylight, 0.0) };
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
    EXPECT_TRUE (std::any_of (unified.mappings.begin (), unified.mappings.end (),
                              [] (const TerrainMapping& row) {
                                  return row.kind == TerrainMappingKind::ControlPointVertex;
                              }));
    EXPECT_EQ (unified.inputProvenance, plan.provenance);
}

TEST (GeometryRustUnifiedTerrain, EmptyPlanWithoutPropertyIsBaselineBitForBit)
{
    const Base base;
    const UnifiedPlanRequest plan { { base.points, base.triangles },
                                    {},
                                    {},
                                    {},
                                    {},
                                    {},
                                    1e-3,
                                    0,
                                    0,
                                    Provenance ({ uint64_t (base.points.size () / 3), 0,
                                                  uint64_t (base.triangles.size () / 3), 0 },
                                                7) };
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
