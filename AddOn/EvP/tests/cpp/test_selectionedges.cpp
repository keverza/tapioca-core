// ArchViz/SelectionEdges -- which edges the selection draws (issue #118).
//
// The live report: a selected terrain showed its whole triangulation, and a column would show
// every facet. Each case below is one of the shapes behind that report, built small enough to read.

#include <gtest/gtest.h>

#include "ArchViz/SelectionEdges.hpp"
#include "ArchViz/SelectionHighlightStyle.hpp"

#include <cmath>
#include <vector>

using geomsrv::archviz::BuildSelectionEdgeMasks;
using geomsrv::archviz::kSelectionCreaseDegrees;

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
    size_t TriangleCount () const
    {
        return indices.size () / 3;
    }
};

// How many distinct edges (by position pair) the masks keep.
size_t KeptEdgeCount (const TestMesh& m, const std::vector<uint8_t>& masks)
{
    static const int corners[3][2] = { { 1, 2 }, { 2, 0 }, { 0, 1 } };
    std::vector<std::vector<float>> kept;
    auto same = [] (const std::vector<float>& a, const std::vector<float>& b) {
        // An edge either way round.
        const bool forward =
            a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
        const bool reverse =
            a[0] == b[3] && a[1] == b[4] && a[2] == b[5] && a[3] == b[0] && a[4] == b[1] && a[5] == b[2];
        return forward || reverse;
    };
    for (size_t t = 0; t < masks.size (); ++t) {
        for (int e = 0; e < 3; ++e) {
            if ((masks[t] & (1u << e)) == 0)
                continue;
            const uint32_t a = m.indices[t * 3 + corners[e][0]];
            const uint32_t b = m.indices[t * 3 + corners[e][1]];
            std::vector<float> edge { m.positions[a * 3], m.positions[a * 3 + 1], m.positions[a * 3 + 2],
                                      m.positions[b * 3], m.positions[b * 3 + 1], m.positions[b * 3 + 2] };
            bool seen = false;
            for (const auto& k : kept)
                seen = seen || same (k, edge);
            if (!seen)
                kept.push_back (edge);
        }
    }
    return kept.size ();
}

// A prism of `sides` facets around the z axis, with both caps fanned from a centre. Each facet
// quad has its OWN corner vertices -- the upload welds by position and normal, so a faceted side
// never shares indices with its neighbour -- and every source edge is "visible", as Archicad
// reports a polygon-per-facet body.
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

} // namespace

TEST (SelectionEdges, FlatQuadKeepsItsOutlineAndDropsItsDiagonal)
{
    TestMesh m;
    const uint32_t a = m.Vertex (0, 0, 0), b = m.Vertex (1, 0, 0), c = m.Vertex (1, 1, 0), d = m.Vertex (0, 1, 0);
    m.Triangle (a, b, c);
    m.Triangle (a, c, d);
    // Even if the source called the diagonal visible, coplanar faces merge.
    const auto masks = BuildSelectionEdgeMasks (m.positions, m.indices, { 0x7u, 0x7u }, kSelectionCreaseDegrees);
    EXPECT_EQ (KeptEdgeCount (m, masks), 4u);
}

TEST (SelectionEdges, HardEdgeIsFoundAcrossSplitVertices)
{
    // Two faces of a box meeting at 90 degrees, each with its own copy of the shared corners.
    TestMesh m;
    const uint32_t a = m.Vertex (0, 0, 0), b = m.Vertex (1, 0, 0), c = m.Vertex (1, 0, 1), d = m.Vertex (0, 0, 1);
    m.Triangle (a, b, c);
    m.Triangle (a, c, d);
    const uint32_t a2 = m.Vertex (0, 0, 0), d2 = m.Vertex (0, 0, 1), e = m.Vertex (0, 1, 1), f = m.Vertex (0, 1, 0);
    m.Triangle (a2, d2, e);
    m.Triangle (a2, e, f);
    const auto masks = BuildSelectionEdgeMasks (m.positions, m.indices, {}, kSelectionCreaseDegrees);
    // Two quads' outlines (4 + 4) share one edge: 7 distinct edges, both diagonals gone.
    EXPECT_EQ (KeptEdgeCount (m, masks), 7u);
}

TEST (SelectionEdges, ManySidedColumnKeepsItsCapsAndLosesItsFacets)
{
    for (int sides : { 16, 32, 64 }) {
        const TestMesh m = Prism (sides);
        const auto masks = BuildSelectionEdgeMasks (m.positions, m.indices, {}, kSelectionCreaseDegrees);
        // The two cap circles, `sides` segments each; no facet line, no spoke.
        EXPECT_EQ (KeptEdgeCount (m, masks), size_t (2 * sides)) << sides << " sides";
    }
}

TEST (SelectionEdges, BoxKeepsEveryFacetEdge)
{
    // A square prism IS its facets: four 90-degree side edges plus the two cap squares.
    const TestMesh m = Prism (4);
    const auto masks = BuildSelectionEdgeMasks (m.positions, m.indices, {}, kSelectionCreaseDegrees);
    EXPECT_EQ (KeptEdgeCount (m, masks), 12u);
}

TEST (SelectionEdges, GentleTerrainShowsOnlyItsBorder)
{
    // A 4x4 grid of quads with a low bump in the middle: every interior edge is a shallow fold.
    TestMesh m;
    const int n = 5;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float h = (x == 2 && y == 2) ? 0.3f : 0.0f;
            m.Vertex (float (x), float (y), h);
        }
    }
    for (int y = 0; y + 1 < n; ++y) {
        for (int x = 0; x + 1 < n; ++x) {
            const uint32_t i = uint32_t (y * n + x);
            m.Triangle (i, i + 1, i + n + 1);
            m.Triangle (i, i + n + 1, i + n);
        }
    }
    const auto masks = BuildSelectionEdgeMasks (m.positions, m.indices, {}, kSelectionCreaseDegrees);
    EXPECT_EQ (KeptEdgeCount (m, masks), 16u) << "the open border, four sides of four";
}

TEST (SelectionEdges, AnEdgeTheWireframeHidesNeverReturns)
{
    // The 90-degree edge of the box above, but the source marks every edge invisible.
    TestMesh m;
    const uint32_t a = m.Vertex (0, 0, 0), b = m.Vertex (1, 0, 0), c = m.Vertex (0, 0, 1);
    m.Triangle (a, b, c);
    const uint32_t a2 = m.Vertex (0, 0, 0), c2 = m.Vertex (0, 0, 1), d = m.Vertex (0, 1, 0);
    m.Triangle (a2, c2, d);
    const auto masks = BuildSelectionEdgeMasks (m.positions, m.indices, { 0u, 0u }, kSelectionCreaseDegrees);
    EXPECT_EQ (KeptEdgeCount (m, masks), 0u);
}

TEST (SelectionEdges, EmptyAndMalformedInputDoNotCrash)
{
    EXPECT_TRUE (BuildSelectionEdgeMasks ({}, {}, {}, kSelectionCreaseDegrees).empty ());
    // An index past the vertex array is skipped, not read.
    const auto masks = BuildSelectionEdgeMasks ({ 0, 0, 0, 1, 0, 0 }, { 0, 1, 7 }, {}, kSelectionCreaseDegrees);
    ASSERT_EQ (masks.size (), 1u);
}
