// Offline checks for ArchViz/StorySliceGeometry — a storey's union outline
// turned into a ribbon and a fill.
//
// The assertions are PROPERTIES, not restatements of the arithmetic: the fill is
// checked by the AREA it covers, the chaining by whether a ring closes, and the
// dash carrier by whether arc length runs continuously along a contour. A test
// that recomputed the same expression would pass for a mirrored ribbon and an
// inside-out fill, which are exactly the two failures that produce a plausible
// picture instead of an error.

#include "ArchViz/StorySliceGeometry.hpp"
#include "ArchViz/StorySliceUnion.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using geomsrv::Polyline;
using geomsrv::archviz::BuildSliceFill;
using geomsrv::archviz::BuildSliceRibbon;
using geomsrv::archviz::ChainUnionSegments;
using geomsrv::archviz::SliceChain;
using geomsrv::archviz::StorySliceFillVertex;
using geomsrv::archviz::StorySliceVertex;
using geomsrv::archviz::UnionArea;
using geomsrv::archviz::UnionLoops;
using geomsrv::archviz::UnionSegment;

namespace {

Polyline Rect (double x0, double y0, double x1, double y1, double z = 0.0)
{
    Polyline p;
    const double xs[5] = { x0, x1, x1, x0, x0 };
    const double ys[5] = { y0, y0, y1, y1, y0 };
    for (int i = 0; i < 5; ++i) {
        p.pts.push_back (xs[i]);
        p.pts.push_back (ys[i]);
        p.pts.push_back (z);
    }
    p.closed = true;
    return p;
}

std::vector<SliceChain> ChainsOf (const std::vector<Polyline>& loops)
{
    return ChainUnionSegments (UnionLoops (loops));
}

double FillArea (const std::vector<SliceChain>& chains)
{
    std::vector<StorySliceFillVertex> tris;
    BuildSliceFill (chains, 0.0f, tris);
    double area = 0.0;
    for (size_t i = 0; i + 2 < tris.size (); i += 3) {
        const double ax = tris[i].x,     ay = tris[i].y;
        const double bx = tris[i + 1].x, by = tris[i + 1].y;
        const double cx = tris[i + 2].x, cy = tris[i + 2].y;
        area += std::fabs ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
    }
    return area;
}

}   // namespace

// ---- chaining --------------------------------------------------------------

TEST (StorySliceGeometry, ASingleRectangleChainsIntoOneClosedRing)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    ASSERT_EQ (chains.size (), 1u);
    EXPECT_TRUE (chains[0].closed);
    EXPECT_EQ (chains[0].Count (), 4u);          // no closing repeat is stored
}

TEST (StorySliceGeometry, DisjointRegionsChainSeparately)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 1, 1), Rect (5, 5, 6, 6) });
    ASSERT_EQ (chains.size (), 2u);
    EXPECT_TRUE (chains[0].closed);
    EXPECT_TRUE (chains[1].closed);
}

TEST (StorySliceGeometry, AnOpenCrossSectionStaysOpen)
{
    // A chain that cannot close must be RETURNED open, not dropped: a silently
    // dropped contour is a hole in the outline that nobody notices.
    std::vector<UnionSegment> segs = { { 0, 0, 1, 0 }, { 1, 0, 1, 1 } };
    const std::vector<SliceChain> chains = ChainUnionSegments (segs);
    ASSERT_EQ (chains.size (), 1u);
    EXPECT_FALSE (chains[0].closed);
    EXPECT_EQ (chains[0].Count (), 3u);
}

TEST (StorySliceGeometry, EmptyInputIsEmptyNotACrash)
{
    EXPECT_TRUE (ChainUnionSegments ({}).empty ());
}

// ---- the ribbon ------------------------------------------------------------

TEST (StorySliceGeometry, RibbonEmitsSixVerticesPerSegment)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    const size_t added = BuildSliceRibbon (chains, 3.25f, ribbon);
    EXPECT_EQ (added, 4u * 6u);                  // a closed quad: 4 segments
    EXPECT_EQ (ribbon.size (), added);
    for (const StorySliceVertex& v : ribbon)
        EXPECT_FLOAT_EQ (v.z, 3.25f);            // the storey's plane, reattached
}

TEST (StorySliceGeometry, RibbonPushDirectionsAreUnitAndPerpendicular)
{
    // ⚠️ THE SHADER DIVIDES BY THE PROJECTED LENGTH OF THESE, so a non-unit
    // direction is not a wrong width — it is a width that varies along the
    // contour, which reads as a tessellation artefact.
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    ASSERT_FALSE (ribbon.empty ());
    for (const StorySliceVertex& v : ribbon) {
        EXPECT_NEAR (std::hypot (v.nx, v.ny), 1.0f, 1e-5f);
        EXPECT_NEAR (std::hypot (v.tx, v.ty), 1.0f, 1e-5f);
        EXPECT_NEAR (v.nx * v.tx + v.ny * v.ty, 0.0f, 1e-5f);
    }
}

TEST (StorySliceGeometry, ArcLengthRunsContinuouslyAroundAContour)
{
    // The dash pattern is a function of this. If it restarted per segment the
    // pattern would stutter at every junction, which reads as z-fighting rather
    // than as a line style.
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 2, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    ASSERT_FALSE (ribbon.empty ());

    float maxArc = 0.0f;
    for (const StorySliceVertex& v : ribbon)
        maxArc = std::max (maxArc, v.arc);
    EXPECT_NEAR (maxArc, 6.0f, 1e-4f);           // the full perimeter 2*(2+1)
}

TEST (StorySliceGeometry, ADegenerateChainAppendsNothing)
{
    SliceChain single;
    single.xy = { 1.0, 2.0 };
    std::vector<StorySliceVertex> ribbon;
    EXPECT_EQ (BuildSliceRibbon ({ single }, 0.0f, ribbon), 0u);
    EXPECT_TRUE (ribbon.empty ());
}

TEST (StorySliceGeometry, RibbonAppendsRatherThanClears)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 1, 1) });
    std::vector<StorySliceVertex> ribbon;
    BuildSliceRibbon (chains, 0.0f, ribbon);
    const size_t first = ribbon.size ();
    BuildSliceRibbon (chains, 3.0f, ribbon);     // a second storey into one buffer
    EXPECT_EQ (ribbon.size (), first * 2);
}

// ---- the fill --------------------------------------------------------------

TEST (StorySliceGeometry, FillCoversARectanglesArea)
{
    EXPECT_NEAR (FillArea (ChainsOf ({ Rect (0, 0, 2, 3) })), 6.0, 1e-6);
}

TEST (StorySliceGeometry, FillOfTwoOverlappingRectanglesIsNotDoubleCounted)
{
    // 3x2 and 3x2 overlapping in a 1x1 corner -> 6 + 6 - 1.
    const double area = FillArea (ChainsOf ({ Rect (0, 0, 3, 2), Rect (2, 1, 5, 3) }));
    EXPECT_NEAR (area, 11.0, 1e-4);
}

TEST (StorySliceGeometry, FillOfAbuttingRectanglesIsTheirSum)
{
    EXPECT_NEAR (FillArea (ChainsOf ({ Rect (0, 0, 2, 2), Rect (2, 0, 4, 2) })), 8.0, 1e-4);
}

TEST (StorySliceGeometry, FillSkipsAHoleUnderEvenOdd)
{
    // ⚠️ THE CASE THE STENCIL TRICK AND THE EAR-CLIP WERE BOTH REJECTED FOR. A
    // wall with an opening cuts an inner ring at the cut plane, and the fill must
    // leave it empty. The rings are handed over with NO orientation guarantee —
    // the union classifies edges, it never orients them — so this can only work
    // by even-odd over the arrangement, which is what the decomposition does.
    SliceChain outer;
    outer.xy = { 0, 0, 10, 0, 10, 10, 0, 10 };
    outer.closed = true;
    SliceChain hole;
    hole.xy = { 3, 3, 6, 3, 6, 6, 3, 6 };
    hole.closed = true;

    EXPECT_NEAR (FillArea ({ outer, hole }), 100.0 - 9.0, 1e-4);
}

TEST (StorySliceGeometry, FillIgnoresOpenChains)
{
    SliceChain open;
    open.xy = { 0, 0, 1, 0, 1, 1 };
    open.closed = false;
    std::vector<StorySliceFillVertex> tris;
    EXPECT_EQ (BuildSliceFill ({ open }, 0.0f, tris), 0u);
}

TEST (StorySliceGeometry, FillHandlesASlantedRegion)
{
    // A triangle, whose trapezoid bands are genuinely slanted rather than
    // axis-aligned: taking the band's X from a single scanline instead of from
    // both its edges would show up here and nowhere else.
    SliceChain tri;
    tri.xy = { 0, 0, 4, 0, 0, 4 };
    tri.closed = true;
    EXPECT_NEAR (FillArea ({ tri }), 8.0, 1e-4);
}

TEST (StorySliceGeometry, FillCarriesTheStoreyPlane)
{
    std::vector<StorySliceFillVertex> tris;
    BuildSliceFill (ChainsOf ({ Rect (0, 0, 1, 1) }), 7.5f, tris);
    ASSERT_FALSE (tris.empty ());
    for (const StorySliceFillVertex& v : tris)
        EXPECT_FLOAT_EQ (v.z, 7.5f);
}

TEST (StorySliceGeometry, UnionAreaAgreesWithTheFillItDescribes)
{
    const std::vector<SliceChain> chains = ChainsOf ({ Rect (0, 0, 3, 2), Rect (2, 1, 5, 3) });
    EXPECT_NEAR (UnionArea (chains), FillArea (chains), 1e-9);
    EXPECT_NEAR (UnionArea (chains), 11.0, 1e-4);
}

TEST (StorySliceGeometry, EmptyChainsFillNothing)
{
    std::vector<StorySliceFillVertex> tris;
    EXPECT_EQ (BuildSliceFill ({}, 0.0f, tris), 0u);
    EXPECT_DOUBLE_EQ (UnionArea ({}), 0.0);
}
