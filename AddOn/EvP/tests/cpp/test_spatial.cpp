#include "MeshFixtures.hpp"
#include "SpatialQueries.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace evptest;
using geomsrv::QueryBox;
using geomsrv::QueryPolygon;
using geomsrv::QuerySphere;

namespace {

bool Contains (const std::vector<std::string>& v, const std::string& s)
{
    return std::find (v.begin (), v.end (), s) != v.end ();
}

// Three unit boxes at x = 0, 10, 20.
geomsrv::Snapshot ThreeBoxes ()
{
    return MakeSnapshot ({ MakeBox ("a", 0, 0, 0),
                           MakeBox ("b", 10, 0, 0),
                           MakeBox ("c", 20, 0, 0) });
}

} // namespace

TEST (SpatialQueries, BoxSelectsOverlappingElementsOnly)
{
    const auto snap = ThreeBoxes ();
    const double mn[3] = { -1, -1, -1 };
    const double mx[3] = { 2, 2, 2 };

    const auto hits = QueryBox (snap, mn, mx);
    ASSERT_EQ (hits.size (), 1u);
    EXPECT_EQ (hits[0], "a");
}

TEST (SpatialQueries, BoxCoveringEverythingSelectsEverything)
{
    const auto snap = ThreeBoxes ();
    const double mn[3] = { -100, -100, -100 };
    const double mx[3] = { 100, 100, 100 };

    EXPECT_EQ (QueryBox (snap, mn, mx).size (), 3u);
}

// Touching, not overlapping: the box's max exactly equals the element's min.
// Aabb::OverlapsBox uses <=, so this counts as a hit — pinning the convention so
// a later "fix" to strict inequality cannot pass silently.
TEST (SpatialQueries, BoxTouchingAFaceCounts)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0) });
    const double mn[3] = { -5, -5, -5 };
    const double mx[3] = { 0, 0, 0 };

    EXPECT_EQ (QueryBox (snap, mn, mx).size (), 1u);
}

TEST (SpatialQueries, BoxMissingEverythingIsEmpty)
{
    const auto snap = ThreeBoxes ();
    const double mn[3] = { 100, 100, 100 };
    const double mx[3] = { 101, 101, 101 };

    EXPECT_TRUE (QueryBox (snap, mn, mx).empty ());
}

TEST (SpatialQueries, SphereSelectsByRadius)
{
    const auto snap = ThreeBoxes ();
    const double centre[3] = { 0.5, 0.5, 0.5 };   // inside box "a"

    EXPECT_EQ (QuerySphere (snap, centre, 0.1).size (), 1u);

    const auto wide = QuerySphere (snap, centre, 12.0);   // reaches b at x=10
    EXPECT_TRUE (Contains (wide, "a"));
    EXPECT_TRUE (Contains (wide, "b"));
    EXPECT_FALSE (Contains (wide, "c"));
}

TEST (SpatialQueries, ZeroRadiusSphereStillMatchesAContainingBox)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0) });
    const double inside[3] = { 0.5, 0.5, 0.5 };
    const double outside[3] = { 5, 5, 5 };

    EXPECT_EQ (QuerySphere (snap, inside, 0.0).size (), 1u);
    EXPECT_TRUE (QuerySphere (snap, outside, 0.0).empty ());
}

TEST (SpatialQueries, PolygonPrismSelectsByFootprintAndZ)
{
    const auto snap = ThreeBoxes ();
    // A square around box "a" only.
    const std::vector<double> poly = { -1,-1,  2,-1,  2,2,  -1,2 };

    const auto hits = QueryPolygon (snap, poly, -1.0, 2.0);
    ASSERT_EQ (hits.size (), 1u);
    EXPECT_EQ (hits[0], "a");
}

TEST (SpatialQueries, PolygonZRangeExcludesElementsOutsideIt)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0) });     // z in [0,1]
    const std::vector<double> poly = { -1,-1,  2,-1,  2,2,  -1,2 };

    EXPECT_EQ (QueryPolygon (snap, poly, 0.0, 1.0).size (), 1u);
    EXPECT_TRUE (QueryPolygon (snap, poly, 5.0, 6.0).empty ());
    EXPECT_TRUE (QueryPolygon (snap, poly, -6.0, -5.0).empty ());
}

// Fewer than 3 points is not a polygon. Documented as ">= 3 points"; the engine
// must reject it rather than read past the end.
TEST (SpatialQueries, DegeneratePolygonsAreRejectedNotCrashed)
{
    const auto snap = ThreeBoxes ();
    EXPECT_NO_FATAL_FAILURE (QueryPolygon (snap, {}, -1, 1));
    EXPECT_NO_FATAL_FAILURE (QueryPolygon (snap, { 0, 0 }, -1, 1));
    EXPECT_NO_FATAL_FAILURE (QueryPolygon (snap, { 0, 0, 1, 1 }, -1, 1));
    // Odd coordinate count — a truncated point.
    EXPECT_NO_FATAL_FAILURE (QueryPolygon (snap, { 0, 0, 1, 0, 1 }, -1, 1));
}

TEST (SpatialQueries, InvertedZRangeDoesNotCrash)
{
    const auto snap = ThreeBoxes ();
    const std::vector<double> poly = { -1,-1,  2,-1,  2,2,  -1,2 };
    EXPECT_NO_FATAL_FAILURE (QueryPolygon (snap, poly, /*zmin=*/5.0, /*zmax=*/-5.0));
}

TEST (SpatialQueries, EmptySnapshotIsEmptyForEveryQuery)
{
    const geomsrv::Snapshot snap;
    const double mn[3] = { -1, -1, -1 };
    const double mx[3] = { 1, 1, 1 };
    const double c[3] = { 0, 0, 0 };

    EXPECT_TRUE (QueryBox (snap, mn, mx).empty ());
    EXPECT_TRUE (QuerySphere (snap, c, 100.0).empty ());
    EXPECT_TRUE (QueryPolygon (snap, { -1,-1, 1,-1, 1,1 }, -1, 1).empty ());
}

// An empty mesh has the default (inverted, invalid) AABB. It must never match.
TEST (SpatialQueries, EmptyMeshWithInvalidAabbNeverMatches)
{
    const auto snap = MakeSnapshot ({ MakeEmpty ("nothing") });
    const double mn[3] = { -1e9, -1e9, -1e9 };
    const double mx[3] = { 1e9, 1e9, 1e9 };

    EXPECT_TRUE (QueryBox (snap, mn, mx).empty ());
}

TEST (SpatialQueries, DegenerateMeshesDoNotCrash)
{
    const auto snap = MakeDegenerateSnapshot ();
    const double mn[3] = { -1e9, -1e9, -1e9 };
    const double mx[3] = { 1e9, 1e9, 1e9 };
    const double c[3] = { 0, 0, 0 };

    EXPECT_NO_FATAL_FAILURE (QueryBox (snap, mn, mx));
    EXPECT_NO_FATAL_FAILURE (QuerySphere (snap, c, 1e6));
    EXPECT_NO_FATAL_FAILURE (QueryPolygon (snap, { -9,-9, 9,-9, 9,9, -9,9 }, -1e9, 1e9));
}
