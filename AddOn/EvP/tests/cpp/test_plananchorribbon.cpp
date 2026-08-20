// PLAT-RE65 — the plan anchor ribbon.
//
// What is worth asserting here is not the arithmetic but the PROPERTIES that
// separate a correct anchor from a plausible wrong one, because every failure
// mode of this file draws a picture rather than raising:
//
//   the ribbon stays ON the outline      (a push direction is perpendicular,
//                                         not parallel — swap them and the line
//                                         runs along itself and disappears)
//   the two sides are OPPOSITE           (get this wrong and the quad is
//                                         degenerate, i.e. an invisible line)
//   a quarter-circle arc lands on its
//   real centre                          (the arc reconstruction, which is the
//                                         only maths here with a convention in it)
//   arcSign MIRRORS the bulge            (the knob the live run turns; if it did
//                                         not actually flip anything, the run
//                                         would settle nothing)

#include "ArchViz/PlanAnchorRibbon.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using geomsrv::archviz::BuildAnchorRibbon;
using geomsrv::archviz::PlanAnchorVertex;
using geomsrv::archviz::TessellateEdge;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A 6 x 0.3 m wall outline, counterclockwise, no closing repeat — the shape
// GetWallPlanOutlines returns for a straight wall.
std::vector<float> WallRing ()
{
    return {0.0f, -0.15f, 6.0f, -0.15f, 6.0f, 0.15f, 0.0f, 0.15f};
}

}   // namespace

TEST (PlanAnchorRibbon, ClosedRingEmitsTwoTrianglesPerSegment)
{
    const std::vector<float> ring = WallRing ();
    std::vector<PlanAnchorVertex> out;

    const size_t added = BuildAnchorRibbon (ring.data (), 4, nullptr, true, 0.0f,
                                            1.0f, 0.05f, out);

    EXPECT_EQ (added, 4u * 6u);        // 4 closing segments, 6 vertices each
    EXPECT_EQ (out.size (), added);
}

TEST (PlanAnchorRibbon, OpenChainHasOneFewerSegmentThanClosed)
{
    const std::vector<float> ring = WallRing ();
    std::vector<PlanAnchorVertex> open, closed;

    BuildAnchorRibbon (ring.data (), 4, nullptr, false, 0.0f, 1.0f, 0.05f, open);
    BuildAnchorRibbon (ring.data (), 4, nullptr, true, 0.0f, 1.0f, 0.05f, closed);

    EXPECT_EQ (open.size () + 6u, closed.size ());
}

TEST (PlanAnchorRibbon, AppendsSoAWholeStoreyIsOneBuffer)
{
    const std::vector<float> ring = WallRing ();
    std::vector<PlanAnchorVertex> out;

    const size_t first = BuildAnchorRibbon (ring.data (), 4, nullptr, true, 0.0f,
                                            1.0f, 0.05f, out);
    const size_t second = BuildAnchorRibbon (ring.data (), 4, nullptr, true, 3.0f,
                                             1.0f, 0.05f, out);

    EXPECT_EQ (out.size (), first + second);
    EXPECT_FLOAT_EQ (out.front ().z, 0.0f);
    EXPECT_FLOAT_EQ (out.back ().z, 3.0f);
}

// ⚠️ THE ONE THAT CATCHES A SWAPPED n/t. Both are unit vectors, so a mix-up
// keeps every count identical and simply draws the line along itself.
TEST (PlanAnchorRibbon, PushDirectionsAreUnitPerpendicularAndOpposite)
{
    const std::vector<float> ring = WallRing ();
    std::vector<PlanAnchorVertex> out;
    BuildAnchorRibbon (ring.data (), 4, nullptr, true, 0.0f, 1.0f, 0.05f, out);

    for (const PlanAnchorVertex& v : out) {
        EXPECT_NEAR (std::sqrt (v.nx * v.nx + v.ny * v.ny), 1.0f, 1e-5f);
        EXPECT_NEAR (std::sqrt (v.tx * v.tx + v.ty * v.ty), 1.0f, 1e-5f);
        EXPECT_NEAR (v.nx * v.tx + v.ny * v.ty, 0.0f, 1e-5f);
    }

    // Segment 0 runs +x, so its across-push is +/-y and its cap-push is +/-x.
    const PlanAnchorVertex& startLeft = out[0];
    const PlanAnchorVertex& startRight = out[2];
    EXPECT_NEAR (startLeft.nx, -startRight.nx, 1e-5f);
    EXPECT_NEAR (startLeft.ny, -startRight.ny, 1e-5f);
    EXPECT_NEAR (std::fabs (startLeft.ny), 1.0f, 1e-5f);
    EXPECT_NEAR (startLeft.tx, -1.0f, 1e-5f);      // the START cap points backwards
}

TEST (PlanAnchorRibbon, RibbonVerticesSitOnTheOutlineItself)
{
    const std::vector<float> ring = WallRing ();
    std::vector<PlanAnchorVertex> out;
    BuildAnchorRibbon (ring.data (), 4, nullptr, true, 0.0f, 1.0f, 0.05f, out);

    // Every vertex position is an outline CORNER: the width is applied by the
    // shader, so nothing here may be pre-offset.
    for (const PlanAnchorVertex& v : out) {
        bool matched = false;
        for (size_t i = 0; i < 4; ++i)
            if (std::fabs (v.x - ring[i * 2]) < 1e-5f && std::fabs (v.y - ring[i * 2 + 1]) < 1e-5f)
                matched = true;
        EXPECT_TRUE (matched) << "vertex (" << v.x << ", " << v.y << ") is not on the outline";
    }
}

TEST (PlanAnchorRibbon, TooFewPointsDrawNothingRatherThanGuessADirection)
{
    const float single[] = {1.0f, 2.0f};
    std::vector<PlanAnchorVertex> out;

    EXPECT_EQ (BuildAnchorRibbon (single, 1, nullptr, true, 0.0f, 1.0f, 0.05f, out), 0u);
    EXPECT_EQ (BuildAnchorRibbon (nullptr, 4, nullptr, true, 0.0f, 1.0f, 0.05f, out), 0u);
    EXPECT_TRUE (out.empty ());
}

TEST (PlanAnchorRibbon, RepeatedPointsAreSkippedNotTurnedIntoNaN)
{
    // A closing repeat left in by mistake, which is the likeliest bad input.
    const float ring[] = {0.0f, 0.0f, 5.0f, 0.0f, 5.0f, 0.0f, 5.0f, 2.0f};
    std::vector<PlanAnchorVertex> out;

    BuildAnchorRibbon (ring, 4, nullptr, true, 0.0f, 1.0f, 0.05f, out);

    EXPECT_EQ (out.size (), 3u * 6u);          // the degenerate segment dropped
    for (const PlanAnchorVertex& v : out) {
        EXPECT_FALSE (std::isnan (v.nx));
        EXPECT_FALSE (std::isnan (v.ny));
    }
}

// ---- the whole set, which is what the viewer is actually handed -------------

TEST (PlanAnchorRibbon, ASetAccumulatesEveryRingIntoOneBuffer)
{
    const std::vector<float> ring = WallRing ();
    const std::vector<std::vector<float>> outlines = {ring, ring, ring};
    std::vector<PlanAnchorVertex> out;

    const size_t added = BuildAnchorRibbonSet (outlines, {}, 0.0f, 1.0f, 0.05f, out);

    EXPECT_EQ (added, 3u * 4u * 6u);
    EXPECT_EQ (out.size (), added);
}

TEST (PlanAnchorRibbon, ASetSkipsDegenerateRingsAndKeepsTheRest)
{
    const std::vector<float> ring = WallRing ();
    const std::vector<std::vector<float>> outlines = {{}, {1.0f, 2.0f}, ring};
    std::vector<PlanAnchorVertex> out;

    EXPECT_EQ (BuildAnchorRibbonSet (outlines, {}, 0.0f, 1.0f, 0.05f, out), 4u * 6u);
}

// ⚠️ THE RULE THAT PROTECTS AGAINST A DISAGREEING WIRE. One angle per point; a
// short array would be read past its end for the last edges. Dropping the arcs
// is the safe reading -- straight edges -- and it must not be padded instead.
TEST (PlanAnchorRibbon, ASetDropsAnArcArrayShorterThanItsPointList)
{
    const std::vector<float> ring = WallRing ();
    const std::vector<std::vector<float>> outlines = {ring};

    std::vector<PlanAnchorVertex> shortArcs, noArcs;
    BuildAnchorRibbonSet (outlines, {{kPi * 0.5f, 0.0f}}, 0.0f, 1.0f, 0.05f, shortArcs);
    BuildAnchorRibbonSet (outlines, {}, 0.0f, 1.0f, 0.05f, noArcs);

    // Identical: the short arc list was ignored, not half-applied. Had it been
    // used, the curved edge would have tessellated into extra segments.
    EXPECT_EQ (shortArcs.size (), noArcs.size ());
}

TEST (PlanAnchorRibbon, ASetAppliesAFullLengthArcArray)
{
    const std::vector<float> ring = WallRing ();
    const std::vector<std::vector<float>> outlines = {ring};
    const std::vector<std::vector<float>> arcs = {{kPi * 0.5f, 0.0f, 0.0f, 0.0f}};

    std::vector<PlanAnchorVertex> curved, straight;
    BuildAnchorRibbonSet (outlines, arcs, 0.0f, 1.0f, 0.05f, curved);
    BuildAnchorRibbonSet (outlines, {}, 0.0f, 1.0f, 0.05f, straight);

    EXPECT_GT (curved.size (), straight.size ());
}

// ---- arcs ------------------------------------------------------------------

// A quarter circle from (1,0) to (0,1) sweeping +90 degrees is centred on the
// ORIGIN with radius 1. That fixes the centre reconstruction completely, which
// is the half of the arc maths that has no convention in it.
TEST (PlanAnchorRibbon, QuarterCircleLandsOnItsRealCentre)
{
    std::vector<float> points;
    TessellateEdge (1.0f, 0.0f, 0.0f, 1.0f, kPi * 0.5f, 1.0f, 0.05f, points);

    ASSERT_GE (points.size (), 4u);
    for (size_t i = 0; i < points.size (); i += 2) {
        const float radius = std::sqrt (points[i] * points[i] +
                                        points[i + 1] * points[i + 1]);
        EXPECT_NEAR (radius, 1.0f, 1e-4f) << "point " << (i / 2) << " is off the circle";
    }
    EXPECT_FLOAT_EQ (points[0], 1.0f);         // starts at the begin point
    EXPECT_FLOAT_EQ (points[1], 0.0f);
}

// ⚠️ THE KNOB THE LIVE RUN TURNS. If arcSign did not really mirror the arc, the
// run could not settle the convention and nobody would find out by looking.
TEST (PlanAnchorRibbon, ArcSignMirrorsTheBulgeAcrossTheChord)
{
    std::vector<float> positive, negative;
    TessellateEdge (1.0f, 0.0f, 0.0f, 1.0f, kPi * 0.5f,  1.0f, 0.05f, positive);
    TessellateEdge (1.0f, 0.0f, 0.0f, 1.0f, kPi * 0.5f, -1.0f, 0.05f, negative);

    ASSERT_EQ (positive.size (), negative.size ());

    // The chord runs (1,0)->(0,1); x+y is 1 on it, < 1 inside, > 1 outside.
    // One sweep must bulge each way, at the same distance.
    const size_t mid = (positive.size () / 4) * 2;
    const float positiveSide = positive[mid] + positive[mid + 1] - 1.0f;
    const float negativeSide = negative[mid] + negative[mid + 1] - 1.0f;

    EXPECT_GT (std::fabs (positiveSide), 1e-3f);
    EXPECT_NEAR (positiveSide, -negativeSide, 1e-4f);
}

TEST (PlanAnchorRibbon, AStraightEdgeIsNotTessellated)
{
    std::vector<float> points;
    TessellateEdge (0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 1.0f, 0.05f, points);

    EXPECT_EQ (points.size (), 2u);            // the start point and nothing else
}

TEST (PlanAnchorRibbon, ACurvedEdgeSubdividesToTheRequestedChord)
{
    std::vector<float> fine, coarse;
    TessellateEdge (1.0f, 0.0f, -1.0f, 0.0f, kPi, 1.0f, 0.02f, fine);
    TessellateEdge (1.0f, 0.0f, -1.0f, 0.0f, kPi, 1.0f, 0.50f, coarse);

    EXPECT_GT (fine.size (), coarse.size ());
    // A half circle of radius 1 is pi long; at 0.5 m chords that is >= 7 points.
    EXPECT_GE (coarse.size () / 2, 6u);
}

// A tessellation that grows without bound on a degenerate request would hang
// the render thread rather than fail, which is the worst shape for a bug here.
TEST (PlanAnchorRibbon, TessellationIsBoundedForADegenerateChordLength)
{
    std::vector<float> points;
    TessellateEdge (1.0f, 0.0f, -1.0f, 0.0f, kPi, 1.0f, 0.0f, points);

    EXPECT_LE (points.size () / 2, 257u);
}
