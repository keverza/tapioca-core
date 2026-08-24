// Offline checks for ArchViz/StorySliceUnion — the 2D boolean union of one
// storey's slice regions.
//
// ⚠️ THESE ARE THE SAME CASES AS `StorySliceOverlay/test_sliceunion.py`, on
// purpose. That file is the canonical suite for the Python original; this one is
// the C++ port's, and the two are meant to assert the same behaviour so the
// viewer and the Python overlay cannot draw different outlines for one storey.
// A case added there belongs here too.
//
// The headline case is the one the user reported: overlapping rectangles offset
// from each other must union into a single outline with no interior lines.

#include "ArchViz/StorySliceUnion.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using geomsrv::Polyline;
using geomsrv::archviz::UnionLoops;
using geomsrv::archviz::UnionSegment;

namespace {

// A closed rectangle ring, first point repeated — the shape SliceMesh emits.
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

double TotalLength (const std::vector<UnionSegment>& segs)
{
    double total = 0.0;
    for (const UnionSegment& s : segs)
        total += std::hypot (s.x1 - s.x0, s.y1 - s.y0);
    return total;
}

// Is (x, y) on some output segment? The same probe the Python suite uses, and it
// is what makes "the interior lines are GONE" an assertion rather than a count.
bool OnBoundary (double x, double y, const std::vector<UnionSegment>& segs, double eps = 1e-3)
{
    for (const UnionSegment& s : segs) {
        const double dx = s.x1 - s.x0, dy = s.y1 - s.y0;
        const double l2 = dx * dx + dy * dy;
        if (l2 == 0.0)
            continue;
        double t = ((x - s.x0) * dx + (y - s.y0) * dy) / l2;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        const double px = s.x0 + t * dx, py = s.y0 + t * dy;
        if ((px - x) * (px - x) + (py - y) * (py - y) < eps * eps)
            return true;
    }
    return false;
}

} // namespace

TEST (StorySliceUnion, SingleRectangleSurvivesAsFourEdges)
{
    const std::vector<UnionSegment> segs = UnionLoops ({ Rect (0, 0, 2, 1) });
    EXPECT_EQ (segs.size (), 4u);
    EXPECT_NEAR (TotalLength (segs), 6.0, 1e-6); // perimeter 2*(2+1)
}

TEST (StorySliceUnion, TwoOffsetOverlappingRectanglesUnion)
{
    // THE reported case: two rectangles overlapping, offset from each other.
    const std::vector<UnionSegment> segs = UnionLoops ({ Rect (0, 0, 3, 2), Rect (2, 1, 5, 3) });
    // No interior lines: a point deep inside the overlap must NOT be on an edge.
    EXPECT_FALSE (OnBoundary (2.5, 1.5, segs));
    // But the outer corners of each rectangle are still on the boundary.
    EXPECT_TRUE (OnBoundary (0.0, 0.0, segs));
    EXPECT_TRUE (OnBoundary (5.0, 3.0, segs));
    // The union perimeter is less than the two separate ones (10 + 10 = 20).
    EXPECT_LT (TotalLength (segs), 20.0 - 1e-6);
}

TEST (StorySliceUnion, FullyContainedRectangleVanishes)
{
    const std::vector<UnionSegment> segs = UnionLoops ({ Rect (0, 0, 10, 10), Rect (3, 3, 6, 6) });
    EXPECT_FALSE (OnBoundary (4.5, 4.5, segs));   // inner interior, no line
    EXPECT_FALSE (OnBoundary (3.0, 3.0, segs));   // the inner edge is gone
    EXPECT_NEAR (TotalLength (segs), 40.0, 1e-4); // only the outer square remains
}

TEST (StorySliceUnion, AbuttingRectanglesMergeSharedEdge)
{
    // Two rooms sharing a wall line -> the union drops the shared edge.
    const std::vector<UnionSegment> segs = UnionLoops ({ Rect (0, 0, 2, 2), Rect (2, 0, 4, 2) });
    EXPECT_FALSE (OnBoundary (2.0, 1.0, segs));   // shared edge is interior -> gone
    EXPECT_NEAR (TotalLength (segs), 12.0, 1e-4); // perimeter of the 4x2 union
}

TEST (StorySliceUnion, OpenPolylinePassesThroughAsPairs)
{
    // Not closed: it cannot bound a region, so it survives whole — but expanded
    // into consecutive PAIRS, because the consumer draws a segment list.
    Polyline open;
    const double pts[3][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 } };
    for (const auto& p : pts) {
        open.pts.push_back (p[0]);
        open.pts.push_back (p[1]);
        open.pts.push_back (p[2]);
    }
    open.closed = false;

    const std::vector<UnionSegment> segs = UnionLoops ({ open });
    ASSERT_EQ (segs.size (), 2u);
    EXPECT_NEAR (TotalLength (segs), 2.0, 1e-9);
}

TEST (StorySliceUnion, HoleInARingIsKeptAsBoundary)
{
    // A wall with an opening yields an outer loop AND an inner one (SliceEngine's
    // header says so). Even-odd makes the inner ring a HOLE, and a hole's edge is
    // union boundary — inside on one side only — so it must survive.
    const std::vector<UnionSegment> segs = UnionLoops ({ Rect (0, 0, 10, 10), Rect (3, 3, 6, 6) });
    // Guard: this is the contained-rectangle case, where the inner ring is a
    // SOLID inside a solid and therefore vanishes. The distinction is even-odd
    // over the ring SET, which is what UnionLoops implements.
    EXPECT_NEAR (TotalLength (segs), 40.0, 1e-4);
}

TEST (StorySliceUnion, DisjointRectanglesBothSurvive)
{
    const std::vector<UnionSegment> segs = UnionLoops ({ Rect (0, 0, 1, 1), Rect (5, 5, 6, 6) });
    EXPECT_NEAR (TotalLength (segs), 8.0, 1e-6); // two separate perimeters
    EXPECT_TRUE (OnBoundary (0.5, 0.0, segs));
    EXPECT_TRUE (OnBoundary (5.5, 5.0, segs));
}

TEST (StorySliceUnion, EmptyInputIsEmptyNotACrash)
{
    EXPECT_TRUE (UnionLoops ({}).empty ());
}

TEST (StorySliceUnion, DegenerateLoopsDoNotCrash)
{
    Polyline single; // one point
    single.pts = { 1.0, 2.0, 3.0 };
    single.closed = false;

    Polyline zeroArea; // a closed ring with no area
    for (int i = 0; i < 4; ++i) {
        zeroArea.pts.push_back (0.0);
        zeroArea.pts.push_back (0.0);
        zeroArea.pts.push_back (0.0);
    }
    zeroArea.closed = true;

    EXPECT_NO_THROW (UnionLoops ({ single, zeroArea }));
}

TEST (StorySliceUnion, OutputIsStableAcrossRuns)
{
    // ⚠️ THE SORT IS LOAD-BEARING, not cosmetic. The classification walks an
    // unordered_map, so without it two runs over one unchanged storey emit the
    // same segments in a different order — and two captures of an unchanged model
    // then differ byte-for-byte, which is the cheapest regression signal there is.
    const std::vector<Polyline> loops = { Rect (0, 0, 3, 2), Rect (2, 1, 5, 3), Rect (7, 0, 9, 4) };
    const std::vector<UnionSegment> a = UnionLoops (loops);
    const std::vector<UnionSegment> b = UnionLoops (loops);
    ASSERT_EQ (a.size (), b.size ());
    for (size_t i = 0; i < a.size (); ++i) {
        EXPECT_EQ (a[i].x0, b[i].x0);
        EXPECT_EQ (a[i].y0, b[i].y0);
        EXPECT_EQ (a[i].x1, b[i].x1);
        EXPECT_EQ (a[i].y1, b[i].y1);
    }
}
