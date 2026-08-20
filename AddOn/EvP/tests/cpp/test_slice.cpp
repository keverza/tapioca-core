#include "MeshFixtures.hpp"
#include "SliceEngine.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace evptest;
using geomsrv::SliceZ;
using geomsrv::SliceResult;

namespace {

const std::vector<int32_t>     kAllTypes;
const std::vector<std::string> kAllGuids;

// Sum of segment lengths around a loop, closing it if the loop is closed.
double Perimeter (const geomsrv::Polyline& p)
{
    const size_t n = p.PointCount ();
    if (n < 2)
        return 0.0;
    double total = 0.0;
    const size_t last = p.closed ? n : n - 1;
    for (size_t i = 0; i < last; ++i) {
        const size_t a = i * 3, b = ((i + 1) % n) * 3;
        total += std::hypot (p.pts[b] - p.pts[a], p.pts[b + 1] - p.pts[a + 1]);
    }
    return total;
}

size_t TotalLoops (const SliceResult& r)
{
    size_t n = 0;
    for (const auto& e : r.elements) n += e.loops.size ();
    return n;
}

} // namespace

// A plane through the middle of a unit box cuts a unit square: the answer is
// arithmetic, not a golden value recorded from this code.
TEST (SliceEngine, MidBoxCutIsAUnitSquare)
{
    const auto snap = MakeSnapshot ({ MakeBox ("box", 0, 0, 0) });
    const SliceResult r = SliceZ (snap, 0.5, kAllTypes, kAllGuids);

    ASSERT_EQ (r.elements.size (), 1u);
    EXPECT_EQ (r.elements[0].guid, "box");
    ASSERT_EQ (r.elements[0].loops.size (), 1u);

    const auto& loop = r.elements[0].loops[0];
    EXPECT_TRUE (loop.closed);
    EXPECT_NEAR (Perimeter (loop), 4.0, 1e-9);

    // Every point sits exactly on the cut plane.
    for (size_t i = 0; i < loop.PointCount (); ++i)
        EXPECT_NEAR (loop.pts[i * 3 + 2], r.zUsed, 1e-9);
}

TEST (SliceEngine, PlaneAboveAndBelowProducesNothing)
{
    const auto snap = MakeSnapshot ({ MakeBox ("box", 0, 0, 0) });

    EXPECT_EQ (TotalLoops (SliceZ (snap, 5.0, kAllTypes, kAllGuids)), 0u);
    EXPECT_EQ (TotalLoops (SliceZ (snap, -5.0, kAllTypes, kAllGuids)), 0u);
}

// The documented tangency rule: cutting exactly at an element's base lifts the
// plane instead of returning an empty (or coplanar-garbage) section. This is the
// "slice at floor level" case the header calls out.
TEST (SliceEngine, CutAtBaseIsNudgedUpAndStillCuts)
{
    const auto snap = MakeSnapshot ({ MakeBox ("box", 0, 0, 0) });
    const SliceResult r = SliceZ (snap, 0.0, kAllTypes, kAllGuids);

    EXPECT_TRUE (r.nudged);
    EXPECT_GT (r.zUsed, 0.0);
    EXPECT_LT (r.zUsed, 1e-3);      // a nudge, not a jump
    EXPECT_EQ (TotalLoops (r), 1u);
}

TEST (SliceEngine, NudgeCanBeDisabled)
{
    const auto snap = MakeSnapshot ({ MakeBox ("box", 0, 0, 0) });
    const SliceResult r = SliceZ (snap, 0.0, kAllTypes, kAllGuids, 1e-6, /*nudge=*/false);

    EXPECT_FALSE (r.nudged);
    EXPECT_DOUBLE_EQ (r.zUsed, 0.0);
}

TEST (SliceEngine, GuidFilterSelectsOneElement)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0), MakeBox ("b", 10, 0, 0) });

    const SliceResult all = SliceZ (snap, 0.5, kAllTypes, kAllGuids);
    EXPECT_EQ (all.elements.size (), 2u);

    const SliceResult one = SliceZ (snap, 0.5, kAllTypes, { "b" });
    ASSERT_EQ (one.elements.size (), 1u);
    EXPECT_EQ (one.elements[0].guid, "b");
}

TEST (SliceEngine, TypeFilterSelectsOneElement)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0, 1, 1, 1, /*elemType=*/7),
                                      MakeBox ("b", 10, 0, 0, 1, 1, 1, /*elemType=*/9) });

    const SliceResult only7 = SliceZ (snap, 0.5, { 7 }, kAllGuids);
    ASSERT_EQ (only7.elements.size (), 1u);
    EXPECT_EQ (only7.elements[0].guid, "a");
}

// Two disjoint solids at the same height are two independent sections.
TEST (SliceEngine, TwoDisjointBoxesGiveTwoLoops)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0), MakeBox ("b", 10, 0, 0) });
    const SliceResult r = SliceZ (snap, 0.5, kAllTypes, kAllGuids);

    EXPECT_EQ (r.elements.size (), 2u);
    EXPECT_EQ (TotalLoops (r), 2u);
}

TEST (SliceEngine, EmptySnapshotIsEmptyNotACrash)
{
    const geomsrv::Snapshot snap;
    const SliceResult r = SliceZ (snap, 0.0, kAllTypes, kAllGuids);
    EXPECT_TRUE (r.elements.empty ());
}

// The point of the degenerate fixtures: no crash, no hang, no OOB — a defined
// result is enough. What that result IS for a NaN triangle is not specified.
TEST (SliceEngine, DegenerateMeshesDoNotCrash)
{
    const auto snap = MakeDegenerateSnapshot ();
    for (const double z : { -1.0, 0.0, 0.5, 1.0, 1e9 }) {
        const SliceResult r = SliceZ (snap, z, kAllTypes, kAllGuids);
        EXPECT_LE (r.elements.size (), snap.meshes.size ());
    }
}

TEST (SliceEngine, WeldToleranceIsAccepted)
{
    const auto snap = MakeSnapshot ({ MakeBox ("box", 0, 0, 0) });
    for (const double weld : { 0.0, 1e-9, 1e-6, 1e-2 }) {
        const SliceResult r = SliceZ (snap, 0.5, kAllTypes, kAllGuids, weld);
        EXPECT_EQ (r.elements.size (), 1u) << "weld=" << weld;
    }
}
