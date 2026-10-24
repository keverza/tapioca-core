#include "Geometry/GeometryRustHybridTerrain.hpp"
#include "GeometryRustFfi.h"
#include <gtest/gtest.h>
#include <bit>
#include <cmath>

namespace {
using namespace tapioca::geometry;

const std::array<double, 12> kRegion { -10, -15, 0, 50, -15, 0, 50, 15, 0, -10, 15, 0 };

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

// 17 x 13 vertices at 5 m on z = 10 + 0.05x - 0.2y, offset off the band's lines.
struct Base {
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    Base ()
    {
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
    }
    EntityCounts Counts () const
    {
        return { points.size () / 3 + 4, 4, triangles.size () / 3, 0 };
    }
};

TerrainStation Station (double x, double z)
{
    TerrainStation station;
    station.position = { x, 0, z };
    station.halfLeft = station.halfRight = 2;
    station.cut = station.fill = { TerrainSlope::Unit::RiseRun, 1, 2 };
    station.cutLimit = station.fillLimit = 10;
    return station;
}
} // namespace

TEST (GeometryRustHybridTerrain, AbiLayouts)
{
    static_assert (sizeof (TapiocaTerrainStation) == 160);
    static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridRequest) == 360);
    static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridOutput) == 144);
    static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridLockedRequest) == 408);
    static_assert (sizeof (TapiocaTerrainHybridDiagnostic) == 80);
    SUCCEED ();
}

TEST (GeometryRustHybridTerrain, FixedControlsRetainTheirCatalogAndEveryCrossingContributor)
{
    const Base base;
    const std::array stations { Station (0, 10), Station (40, 12) };
    const std::array<std::vector<double>, 2> lines { std::vector<double> { 5, -1, 10.25, 35, 1, 11.75 },
                                                     std::vector<double> { 5, 1, 10.25, 35, -1, 11.75 } };
    const std::vector<double> points { 20, 0, 11, 20, 0, 11 };
    auto counts = base.Counts ();
    counts.vertices += 6;
    counts.edges += 2;
    auto provenance = Provenance (counts, 7);
    const HybridTerrainRequest request {
        { base.points, base.triangles }, kRegion, stations, TerrainEditEffect::Both, 1e-6, provenance, lines, points
    };
    HybridTerrainResult result;
    std::string error;
    ASSERT_TRUE (BuildHybridTerrain (request, result, error)) << error;
    EXPECT_EQ (result.inputProvenance, provenance);
    EXPECT_EQ (result.inputRegionCount, 4U);
    EXPECT_EQ (result.inputControlLineStarts,
               (std::vector<size_t> { base.points.size () / 3 + 4, base.points.size () / 3 + 6 }));
    EXPECT_EQ (result.inputControlPointStart, base.points.size () / 3 + 8);
    ASSERT_GT (result.locked.size (), 5U);
    bool shared = false;
    for (const auto& vertex : result.locked) {
        EXPECT_FALSE (result.owners.at (vertex.vertex));
        EXPECT_NEAR (result.points.at (vertex.vertex * 3 + 2), 10 + .05 * result.points.at (vertex.vertex * 3), 1e-12);
        shared |= vertex.supports.size () == 4;
    }
    EXPECT_TRUE (shared);
    EXPECT_EQ (result.lines.front ().kind, HybridLineKind::Locked);
    auto broken = provenance;
    broken.counts.vertices -= 1;
    const auto before = result;
    EXPECT_FALSE (BuildHybridTerrain (
        { request.mesh, kRegion, stations, request.effect, request.tolerance, broken, lines, points }, result, error));
    EXPECT_EQ (result, before);
    EXPECT_NE (error.find ("Provenance"), std::string::npos);
}

TEST (GeometryRustHybridTerrain, FixedConflictAssessmentCarriesInputLocatorsWithoutPublishingAMesh)
{
    const Base base;
    const std::array stations { Station (0, 10), Station (40, 12) };
    const std::vector<double> points { 20, 0, 12 };
    auto counts = base.Counts ();
    ++counts.vertices;
    const auto provenance = Provenance (counts, 7);
    const HybridTerrainRequest request {
        { base.points, base.triangles }, kRegion, stations, TerrainEditEffect::Both, 1e-6, provenance, {}, points
    };
    HybridTerrainResult result;
    result.points = { 77 };
    const auto before = result;
    std::string error;
    EXPECT_FALSE (BuildHybridTerrain (request, result, error));
    EXPECT_EQ (result, before);
    EXPECT_EQ (error.rfind ("LockedConflict:", 0), 0U);
    std::vector<HybridConflict> conflicts;
    ASSERT_TRUE (AssessHybridTerrain (request, conflicts, error)) << error;
    ASSERT_EQ (conflicts.size (), 1U);
    const auto& conflict = conflicts.front ();
    EXPECT_EQ (conflict.code, HybridConflictCode::LockedConflict);
    EXPECT_EQ (conflict.control, (std::optional<HybridControlRef> { { HybridControlRef::Kind::Point, 0, 0 } }));
    EXPECT_FALSE (conflict.other);
    EXPECT_EQ (conflict.station, UINT32_MAX);
    EXPECT_EQ (conflict.position, (std::array<double, 3> { 20, 0, 12 }));
    EXPECT_NEAR (conflict.magnitude, -1, 1e-12);
}

TEST (GeometryRustHybridTerrain, FixedAssessmentTransportsAllCanonicalConflictsInStableOrder)
{
    const Base base;
    const std::array stations { Station (0, 10), Station (40, 12) };
    const std::vector<double> points { 10, 0, 10.5, 10, 0, 11.5, 10, 0, 12.5, 20, 0, 12, 30, 0, 12.5 };
    auto counts = base.Counts ();
    counts.vertices += 5;
    const auto provenance = Provenance (counts, 7);
    const HybridTerrainRequest request {
        { base.points, base.triangles }, kRegion, stations, TerrainEditEffect::Both, 1e-6, provenance, {}, points
    };
    std::vector<HybridConflict> result;
    std::string error;
    ASSERT_TRUE (AssessHybridTerrain (request, result, error)) << error;
    ASSERT_EQ (result.size (), 4U);
    for (size_t i = 0; i < result.size (); ++i) {
        const auto& conflict = result[i];
        EXPECT_EQ (conflict.code, HybridConflictCode::LockedConflict);
        EXPECT_TRUE (conflict.hard);
        ASSERT_TRUE (conflict.control);
        EXPECT_EQ (conflict.control->kind, HybridControlRef::Kind::Point);
        EXPECT_EQ (conflict.control->index, i < 2 ? 0U : static_cast<uint32_t> (i + 1));
        if (i < 2) {
            ASSERT_TRUE (conflict.other);
            EXPECT_EQ (conflict.other->index, i + 1);
            EXPECT_EQ (conflict.position, (std::array<double, 3> { 10, 0, 10.5 }));
        }
        else
            EXPECT_FALSE (conflict.other);
    }
    std::vector<HybridConflict> again;
    ASSERT_TRUE (AssessHybridTerrain (request, again, error)) << error;
    EXPECT_EQ (again, result);
    auto bad = provenance;
    ++bad.expectedRevision;
    EXPECT_FALSE (AssessHybridTerrain (
        { request.mesh, kRegion, stations, request.effect, request.tolerance, bad, {}, points }, result, error));
    EXPECT_EQ (result, again);
}

TEST (GeometryRustHybridTerrain, SparseSourceGetsTransverseStationDetailWithRetainedProvenance)
{
    const std::vector<double> points { -20, -30, 10, 60, -30, 10, 60, 30, 10, -20, 30, 10 };
    const std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
    auto stations =
        std::array { Station (0, 11), Station (18, 11), Station (20, 13), Station (22, 11), Station (40, 11) };
    stations[2].halfLeft = stations[2].halfRight = 8;
    const auto provenance = Provenance ({ 8, 4, 2, 0 }, 7);
    HybridTerrainResult result;
    std::string error;
    ASSERT_TRUE (BuildHybridTerrain (
        { { points, triangles }, kRegion, stations, TerrainEditEffect::Both, 1e-6, provenance }, result, error))
        << error;
    EXPECT_EQ (result.inputProvenance, provenance);
    EXPECT_EQ (result.inputPointCount, 4U);
    EXPECT_GT (result.points.size (), points.size ());
    EXPECT_GT (result.triangles.size (), triangles.size ());
    std::vector<uint32_t> sections;
    for (const auto& line : result.lines)
        if (line.kind == HybridLineKind::Section) {
            EXPECT_FALSE (line.side);
            sections.push_back (line.root);
        }
    EXPECT_EQ (sections, (std::vector<uint32_t> { 1, 2, 3 }));
    const std::array<double, 10> queries { 20, -7, 20, -3, 20, 0, 20, 3, 20, 7 };
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    ASSERT_TRUE (SampleTerrain ({ result.points, result.triangles }, queries, .25, samples, summary, error)) << error;
    for (const auto& sample : samples) {
        EXPECT_TRUE (sample.onSurface);
        EXPECT_NEAR (sample.height, 13, 1e-9);
    }
    EXPECT_EQ (result.context.cut, 0);
    EXPECT_EQ (result.context.fill, 0);
}

TEST (GeometryRustHybridTerrain, WorkedBandBuildsWithExactDaylightAndUntouchedOutside)
{
    const Base base;
    const auto provenance = Provenance (base.Counts (), 7);
    const std::array stations { Station (0, 10), Station (40, 12) };
    HybridTerrainResult result;
    std::string error;
    ASSERT_TRUE (BuildHybridTerrain (
        { { base.points, base.triangles }, kRegion, stations, TerrainEditEffect::Both, 1e-6, provenance }, result,
        error))
        << error;
    EXPECT_EQ (result.inputProvenance, provenance);
    EXPECT_EQ (result.inputPointCount, base.points.size () / 3);
    // The input vertices are an unchanged prefix of the rebuilt base.
    ASSERT_GE (result.points.size (), base.points.size ());
    const std::vector<HybridLine> lines {
        { HybridLineKind::Edge, TerrainHand::Left, 0 },  { HybridLineKind::Daylight, TerrainHand::Left, 0 },
        { HybridLineKind::Edge, TerrainHand::Right, 0 }, { HybridLineKind::Daylight, TerrainHand::Right, 0 },
        { HybridLineKind::End, std::nullopt, 0 },        { HybridLineKind::End, std::nullopt, 1 }
    };
    EXPECT_EQ (result.lines, lines);
    ASSERT_EQ (result.sides.size (), 2U);
    for (const auto& pair : result.sides)
        for (const auto& side : pair) {
            EXPECT_EQ (side.roots, 1U);
            EXPECT_NEAR (side.first, 4.0 / 3.0, 1e-9);
        }
    // Site totals include the unconstrained end caps (S4.1 evidence).
    EXPECT_NEAR (result.site.fill, 27.9444, 1e-3);
    EXPECT_NEAR (result.site.cut, 27.7778, 1e-3);
    EXPECT_EQ (result.context.fill, 0.0);
    EXPECT_EQ (result.context.cut, 0.0);
    ASSERT_EQ (result.base.size (), result.points.size () / 3);
    ASSERT_EQ (result.owners.size (), result.base.size ());
    size_t moved = 0;
    for (size_t v = 0; v < result.base.size (); ++v) {
        const double x = result.points[v * 3], y = result.points[v * 3 + 1], z = result.points[v * 3 + 2];
        if (x < -10 || x > 50 || std::abs (y) > 15) {
            EXPECT_EQ (std::bit_cast<uint64_t> (z), std::bit_cast<uint64_t> (result.base[v]));
            EXPECT_FALSE (result.owners[v].has_value ());
        }
        moved += result.owners[v].has_value ();
    }
    EXPECT_GT (moved, 0U);
    size_t faces = 0, ring = 0;
    for (const auto& row : result.mappings) {
        faces += row.kind == TerrainMappingKind::RegionFace;
        ring += row.kind == TerrainMappingKind::RingVertex;
    }
    EXPECT_EQ (faces * 3, result.triangles.size ());
    EXPECT_GE (ring, 4U);
}

TEST (GeometryRustHybridTerrain, RefusalsKeepTheOwnedResult)
{
    const Base base;
    const auto provenance = Provenance (base.Counts (), 7);
    HybridTerrainResult result;
    result.points = { 77 };
    const auto before = result;
    std::string error;
    auto unmet = std::array { Station (0, 10), Station (40, 12) };
    unmet[0].cutLimit = unmet[0].fillLimit = .5;
    EXPECT_FALSE (BuildHybridTerrain (
        { { base.points, base.triangles }, kRegion, unmet, TerrainEditEffect::Both, 1e-6, provenance }, result, error));
    EXPECT_EQ (error.rfind ("DaylightNotReached", 0), 0U) << error;
    EXPECT_EQ (result, before);
    const std::array stations { Station (0, 10), Station (40, 12) };
    const auto wrong = Provenance ({ base.points.size () / 3, 0, base.triangles.size () / 3, 0 }, 5);
    EXPECT_FALSE (BuildHybridTerrain (
        { { base.points, base.triangles }, kRegion, stations, TerrainEditEffect::Both, 1e-6, wrong }, result, error));
    EXPECT_EQ (error.rfind ("ProvenanceConflict", 0), 0U) << error;
    EXPECT_EQ (result, before);
    auto both = std::array { Station (0, 10), Station (40, 12) };
    both[0].crown = TerrainSlope { TerrainSlope::Unit::Percent, 2, 0 };
    both[0].crossfall = TerrainCrossfall { { TerrainSlope::Unit::Percent, 2, 0 }, TerrainHand::Left };
    EXPECT_FALSE (BuildHybridTerrain (
        { { base.points, base.triangles }, kRegion, both, TerrainEditEffect::Both, 1e-6, provenance }, result, error));
    EXPECT_EQ (error.rfind ("TerrainHybridCamber", 0), 0U) << error;
    EXPECT_EQ (result, before);
}

TEST (GeometryRustHybridTerrain, ConflictsAreLocatedForRefusedAndRestrictedBands)
{
    const Base base;
    const auto provenance = Provenance (base.Counts (), 7);
    auto shortReach = std::array { Station (0, 10), Station (40, 12) };
    for (auto& station : shortReach)
        station.cutLimit = station.fillLimit = .5;
    const HybridTerrainRequest refused { { base.points, base.triangles }, kRegion, shortReach,
                                         TerrainEditEffect::Both,         1e-6,    provenance };
    HybridTerrainResult result;
    std::string error;
    EXPECT_FALSE (BuildHybridTerrain (refused, result, error));
    std::vector<HybridConflict> conflicts;
    ASSERT_TRUE (AssessHybridTerrain (refused, conflicts, error)) << error;
    ASSERT_EQ (conflicts.size (), 4U);
    for (const auto& conflict : conflicts) {
        EXPECT_EQ (conflict.code, HybridConflictCode::DaylightBeyondLimit);
        EXPECT_TRUE (conflict.hard);
        EXPECT_NEAR (conflict.magnitude, 4.0 / 3.0, 1e-9);
        EXPECT_NEAR (std::abs (conflict.position[1]), 2.5, 1e-12);
    }
    EXPECT_STREQ (HybridConflictName (conflicts[0].code), "DaylightBeyondLimit");
    // Fill only: the right edge is in cut, built and reported soft.
    const std::array stations { Station (0, 10), Station (40, 12) };
    ASSERT_TRUE (BuildHybridTerrain (
        { { base.points, base.triangles }, kRegion, stations, TerrainEditEffect::Fill, 1e-6, provenance }, result,
        error))
        << error;
    ASSERT_EQ (result.conflicts.size (), 2U);
    for (const auto& conflict : result.conflicts) {
        EXPECT_EQ (conflict.code, HybridConflictCode::EdgeContradictsEffect);
        EXPECT_EQ (conflict.side, TerrainHand::Right);
        EXPECT_FALSE (conflict.hard);
        EXPECT_NEAR (conflict.magnitude, -.4, 1e-12);
    }
    // An attribution refusal leaves the list unchanged.
    const auto wrong = Provenance ({ base.points.size () / 3, 0, base.triangles.size () / 3, 0 }, 5);
    const auto before = conflicts;
    EXPECT_FALSE (AssessHybridTerrain (
        { { base.points, base.triangles }, kRegion, stations, TerrainEditEffect::Both, 1e-6, wrong }, conflicts,
        error));
    EXPECT_EQ (conflicts, before);
}
