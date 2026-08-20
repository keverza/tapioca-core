#include "MeshFixtures.hpp"
#include "ClashEngine.hpp"

#include <gtest/gtest.h>

using namespace evptest;
using geomsrv::ClashAll;
using geomsrv::Clearance;
using geomsrv::MeshClearance;
using geomsrv::MeshesClash;

TEST (ClashEngine, OverlappingBoxesClash)
{
    const Mesh a = MakeBox ("a", 0, 0, 0);
    const Mesh b = MakeBox ("b", 0.5, 0.5, 0.5);   // corner-overlaps a
    EXPECT_TRUE (MeshesClash (a, b));
    EXPECT_TRUE (MeshesClash (b, a));              // symmetric
}

TEST (ClashEngine, FarApartBoxesDoNotClash)
{
    const Mesh a = MakeBox ("a", 0, 0, 0);
    const Mesh b = MakeBox ("b", 10, 0, 0);
    EXPECT_FALSE (MeshesClash (a, b));
}

// A box fully inside another has no INTERSECTING triangles. Whatever the engine
// answers, it must answer the same way for both argument orders — an asymmetric
// containment result is the bug worth catching here.
TEST (ClashEngine, ContainmentIsSymmetric)
{
    const Mesh outer = MakeBox ("outer", 0, 0, 0, 10, 10, 10);
    const Mesh inner = MakeBox ("inner", 4, 4, 4, 1, 1, 1);
    EXPECT_EQ (MeshesClash (outer, inner), MeshesClash (inner, outer));
}

TEST (ClashEngine, ClearanceOfSeparatedBoxesIsTheGap)
{
    // a spans x in [0,1]; b spans x in [3,4]. Surface-to-surface gap = 2.
    const Mesh a = MakeBox ("a", 0, 0, 0);
    const Mesh b = MakeBox ("b", 3, 0, 0);

    const Clearance c = MeshClearance (a, b);
    EXPECT_FALSE (c.clash);
    EXPECT_NEAR (c.dist, 2.0, 1e-9);
}

TEST (ClashEngine, ClearanceOfClashingBoxesIsZero)
{
    const Mesh a = MakeBox ("a", 0, 0, 0);
    const Mesh b = MakeBox ("b", 0.5, 0.5, 0.5);

    const Clearance c = MeshClearance (a, b);
    EXPECT_TRUE (c.clash);
    EXPECT_DOUBLE_EQ (c.dist, 0.0);
}

// Face-to-face contact: the classic epsilon case. Distance must be 0, and the
// engine must not report a negative or NaN distance.
TEST (ClashEngine, TouchingFacesHaveZeroClearance)
{
    const Mesh a = MakeBox ("a", 0, 0, 0);
    const Mesh b = MakeBox ("b", 1, 0, 0);   // shares the x=1 face

    const Clearance c = MeshClearance (a, b);
    EXPECT_NEAR (c.dist, 0.0, 1e-9);
    EXPECT_GE (c.dist, 0.0);
    EXPECT_FALSE (std::isnan (c.dist));
}

TEST (ClashEngine, ClearanceWitnessPointsLieOnTheirOwnMesh)
{
    const Mesh a = MakeBox ("a", 0, 0, 0);
    const Mesh b = MakeBox ("b", 3, 0, 0);

    const Clearance c = MeshClearance (a, b);
    ASSERT_FALSE (c.clash);
    // Witness on A is on A's +X face, witness on B on B's -X face.
    EXPECT_NEAR (c.pointA[0], 1.0, 1e-9);
    EXPECT_NEAR (c.pointB[0], 3.0, 1e-9);
}

TEST (ClashEngine, ClashAllFindsOnlyTheClashingPair)
{
    const auto snap = MakeSnapshot ({
        MakeBox ("a", 0, 0, 0),
        MakeBox ("b", 0.5, 0.5, 0.5),   // clashes with a
        MakeBox ("c", 50, 0, 0),        // far from both
    });

    const auto pairs = ClashAll (snap, /*gap=*/0.0);
    ASSERT_EQ (pairs.size (), 1u);
    EXPECT_EQ (pairs[0].i, 0u);
    EXPECT_EQ (pairs[0].j, 1u);
}

// gap > 0 must widen the result to near-misses, never narrow it.
TEST (ClashEngine, PositiveGapIncludesNearMisses)
{
    const auto snap = MakeSnapshot ({
        MakeBox ("a", 0, 0, 0),
        MakeBox ("b", 2, 0, 0),   // 1.0 away from a
    });

    EXPECT_EQ (ClashAll (snap, 0.0).size (), 0u);
    EXPECT_EQ (ClashAll (snap, 0.5).size (), 0u);   // gap smaller than the 1.0 distance
    EXPECT_EQ (ClashAll (snap, 1.5).size (), 1u);   // gap larger -> reported
}

TEST (ClashEngine, ClashAllOnEmptySnapshotIsEmpty)
{
    const geomsrv::Snapshot snap;
    EXPECT_TRUE (ClashAll (snap, 0.0).empty ());
    EXPECT_TRUE (ClashAll (snap, 10.0).empty ());
}

TEST (ClashEngine, DegenerateMeshesDoNotCrash)
{
    const auto snap = MakeDegenerateSnapshot ();
    EXPECT_NO_FATAL_FAILURE (ClashAll (snap, 0.0));
    EXPECT_NO_FATAL_FAILURE (ClashAll (snap, 1.0));

    for (const auto& m : snap.meshes) {
        EXPECT_NO_FATAL_FAILURE (MeshesClash (m, m));
        EXPECT_NO_FATAL_FAILURE (MeshClearance (m, m));
    }
}

TEST (ClashEngine, EmptyMeshNeverClashes)
{
    const Mesh empty = MakeEmpty ();
    const Mesh box = MakeBox ("box", 0, 0, 0);
    EXPECT_FALSE (MeshesClash (empty, box));
    EXPECT_FALSE (MeshesClash (box, empty));
    EXPECT_FALSE (MeshesClash (empty, empty));
}
