// ArchViz/TechnicalEdges + BuildCreaseEdgeMasks -- the model-wide technical edges (issue #14 P2).
//
// The edge angle is a choice of tiers packed at packet build; the risk is a tier that silently
// reads the wrong bits (the selection's edges change) or a threshold that keeps the triangulation.
// A prism's facet angle is 360/sides, so the sides decide exactly which tiers keep its facets.

#include <gtest/gtest.h>

#include "ArchViz/SelectionEdges.hpp"
#include "ArchViz/TechnicalEdges.hpp"

#include <cmath>
#include <vector>

using namespace geomsrv::archviz;

namespace {

struct TestMesh {
    std::vector<float> positions;
    std::vector<uint32_t> indices;

    uint32_t Vertex (float x, float y, float z)
    {
        positions.insert (positions.end (), { x, y, z });
        return uint32_t (positions.size () / 3 - 1);
    }
    void Triangle (uint32_t a, uint32_t b, uint32_t c)
    {
        indices.insert (indices.end (), { a, b, c });
    }
};

// The same prism as test_selectionedges.cpp: `sides` facets, each with its own corners, both caps
// fanned from a centre.
TestMesh Prism (int sides)
{
    TestMesh m;
    auto ring = [&] (int i, float z) {
        const float a = 2.0f * 3.14159265f * float (i % sides) / float (sides);
        return m.Vertex (std::cos (a), std::sin (a), z);
    };
    for (int i = 0; i < sides; ++i) {
        const uint32_t b0 = ring (i, 0.0f), b1 = ring (i + 1, 0.0f), t0 = ring (i, 1.0f), t1 = ring (i + 1, 1.0f);
        m.Triangle (b0, b1, t1);
        m.Triangle (b0, t1, t0);
    }
    const uint32_t bottom = m.Vertex (0.0f, 0.0f, 0.0f);
    const uint32_t top = m.Vertex (0.0f, 0.0f, 1.0f);
    for (int i = 0; i < sides; ++i) {
        m.Triangle (bottom, ring (i + 1, 0.0f), ring (i, 0.0f));
        m.Triangle (top, ring (i, 1.0f), ring (i + 1, 1.0f));
    }
    return m;
}

// Whether the vertical facet edge of the first side quad (triangle 0's edge (v2,v0) is the
// diagonal; triangle 1's edge (v2,v0) = (t0,b0) is the vertical) is kept in a 3-bit mask.
bool KeepsFacetEdge (uint32_t mask3)
{
    return (mask3 & 0x2u) != 0;
}

std::vector<uint32_t> AllTiers (const TestMesh& m)
{
    return BuildCreaseEdgeMasks (m.positions, m.indices, {}, kCreaseTierDegrees, kCreaseTierCount);
}

uint32_t Tier (uint32_t packed, size_t tier)
{
    return (packed >> (3 * tier)) & 0x7u;
}

} // namespace

TEST (TechnicalEdges, TierZeroIsExactlyTheSelectionsEdges)
{
    for (const int sides : { 4, 8, 16, 32 }) {
        const TestMesh m = Prism (sides);
        const std::vector<uint8_t> selection =
            BuildSelectionEdgeMasks (m.positions, m.indices, {}, kSelectionCreaseDegrees);
        const std::vector<uint32_t> packed = AllTiers (m);
        ASSERT_EQ (selection.size (), packed.size ());
        for (size_t t = 0; t < packed.size (); ++t)
            EXPECT_EQ (uint32_t (selection[t]), Tier (packed[t], 0)) << "sides " << sides << " tri " << t;
    }
    EXPECT_EQ (CreaseTierShift (0), kSelectionEdgeShift);
}

TEST (TechnicalEdges, FacetAngleDecidesWhichTiersKeepTheFacets)
{
    // 16 sides: facets meet at 22.5 degrees -> kept at 10 and 20, merged at 30, 40 and 60.
    const std::vector<uint32_t> packed = AllTiers (Prism (16));
    const uint32_t tri = packed[1];
    for (size_t k = 0; k < kCreaseTierCount; ++k) {
        const bool expected = kCreaseTierDegrees[k] < 22.5f;
        EXPECT_EQ (KeepsFacetEdge (Tier (tri, k)), expected) << kCreaseTierDegrees[k] << " degrees";
    }
}

TEST (TechnicalEdges, BoxFacetsSurviveEveryTier)
{
    const std::vector<uint32_t> packed = AllTiers (Prism (4)); // 90-degree corners
    for (size_t k = 0; k < kCreaseTierCount; ++k)
        EXPECT_TRUE (KeepsFacetEdge (Tier (packed[1], k))) << kCreaseTierDegrees[k] << " degrees";
}

TEST (TechnicalEdges, TheDiagonalNeverReturnsAtAnyTier)
{
    // Triangle 0's edge (v2,v0) is the side quad's diagonal: coplanar faces, never an edge.
    for (const int sides : { 4, 16 }) {
        const std::vector<uint32_t> packed = AllTiers (Prism (sides));
        for (size_t k = 0; k < kCreaseTierCount; ++k)
            EXPECT_EQ (Tier (packed[0], k) & 0x2u, 0u) << "sides " << sides << ", " << kCreaseTierDegrees[k];
    }
}

TEST (TechnicalEdges, TiersFitTheWireMaskAndDegenerateInputIsSafe)
{
    EXPECT_LE (CreaseTierShift (kCreaseTierCount - 1) + 3u, 32u);
    EXPECT_TRUE (BuildCreaseEdgeMasks ({}, {}, {}, kCreaseTierDegrees, kCreaseTierCount).empty ());
    const TestMesh m = Prism (4);
    const std::vector<uint32_t> none = BuildCreaseEdgeMasks (m.positions, m.indices, {}, nullptr, 3);
    for (uint32_t mask : none)
        EXPECT_EQ (mask, 0u);
}

TEST (TechnicalEdges, EveryHudAngleIsATier)
{
    for (size_t k = 0; k < kTechnicalEdgeAngleCount; ++k)
        EXPECT_FLOAT_EQ (kCreaseTierDegrees[NearestCreaseTier (kTechnicalEdgeAngles[k])], kTechnicalEdgeAngles[k]);
    EXPECT_EQ (NearestCreaseTier (kSelectionCreaseDegrees), 0u);
    EXPECT_FLOAT_EQ (kCreaseTierDegrees[NearestCreaseTier (24.0f)], 20.0f);
    EXPECT_FLOAT_EQ (kCreaseTierDegrees[NearestCreaseTier (500.0f)], 60.0f);
}

TEST (TechnicalEdges, HudStyleDecodesColourAndPicksTheShift)
{
    const TechnicalEdgeStyle style = TechnicalEdgeStyleFromHud (true, 0x808080FFu, 10.0f, true, 30.0f);
    EXPECT_TRUE (style.enabled);
    EXPECT_TRUE (style.showHidden);
    EXPECT_NEAR (style.rgba[0], 0.2158f, 1.0e-3f); // sRGB 0x80 -> linear, not 0.5
    EXPECT_FLOAT_EQ (style.rgba[3], 1.0f);
    EXPECT_FLOAT_EQ (style.pixels, 6.0f); // clamped
    EXPECT_EQ (style.maskShift, CreaseTierShift (NearestCreaseTier (30.0f)));
    EXPECT_NE (style.maskShift, kSelectionEdgeShift);
}
