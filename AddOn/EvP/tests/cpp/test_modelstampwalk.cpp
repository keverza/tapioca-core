// The model watch's per-edit detector (ArchViz/ModelStampWalk, #97): what a
// walk reports, which neighbours an edit drags along, and when the 3D generator
// still runs.

#include "ArchViz/ModelStampWalk.hpp"

#include <gtest/gtest.h>

namespace walk = geomsrv::archviz::stampwalk;

namespace {

walk::Box MakeBox (double x0, double y0, double z0, double x1, double y1, double z1)
{
    walk::Box box;
    box.lo[0] = x0, box.lo[1] = y0, box.lo[2] = z0;
    box.hi[0] = x1, box.hi[1] = y1, box.hi[2] = z1;
    return box;
}

} // namespace

TEST (ModelStampWalk, AWalkReportsNewChangedAndGoneElements)
{
    const walk::Stamps before { { "a", 1 }, { "b", 2 }, { "c", 3 } };
    const walk::Stamps after { { "a", 1 }, { "b", 9 }, { "d", 4 } };
    geomsrv::modeldiff::Result diff;
    walk::Compare (before, after, diff);
    EXPECT_EQ (diff.created, std::vector<std::string> { "d" });
    EXPECT_EQ (diff.modified, std::vector<std::string> { "b" });
    EXPECT_EQ (diff.deleted, std::vector<std::string> { "c" });
    EXPECT_FALSE (diff.environmentChanged) << "the walk never claims the environment moved";
}

// A moved window's host keeps its own stamp; it must still be re-extracted.
TEST (ModelStampWalk, ElementsTouchingTheChangedBoxGoTooUpToTheCap)
{
    const std::vector<walk::Box> changed { MakeBox (0, 0, 0, 1, 1, 1) };
    const std::vector<std::pair<std::string, walk::Box>> candidates {
        { "edited", MakeBox (0, 0, 0, 1, 1, 1) },  { "host", MakeBox (0.5, -1, 0, 3, 2, 3) },
        { "near", MakeBox (1.04, 0, 0, 2, 1, 1) }, { "far", MakeBox (5, 5, 5, 6, 6, 6) },
        { "above", MakeBox (0, 0, 1.2, 1, 1, 2) },
    };
    size_t dropped = 0;
    auto out = walk::Neighbours (changed, candidates, { "edited" }, walk::kNeighbourToleranceM, 8, dropped);
    EXPECT_EQ (out, (std::vector<std::string> { "host", "near" }));
    EXPECT_EQ (dropped, 0u);

    out = walk::Neighbours (changed, candidates, { "edited" }, walk::kNeighbourToleranceM, 1, dropped);
    EXPECT_EQ (out.size (), 1u);
    EXPECT_EQ (dropped, 1u) << "past the cap is left to the backstop, and counted";
}

TEST (ModelStampWalk, APublishedMeshBoxSpansItsVertices)
{
    geomsrv::Snapshot snapshot;
    geomsrv::Mesh mesh;
    mesh.guid = "w";
    mesh.vertices = { 1, 2, 3, -1, 5, 0, 4, 0, 7 };
    snapshot.meshes.push_back (mesh);
    geomsrv::Mesh empty;
    empty.guid = "none";
    snapshot.meshes.push_back (empty);
    const auto boxes = walk::MeshBoxes (snapshot);
    ASSERT_EQ (boxes.size (), 1u);
    EXPECT_EQ (boxes[0].first, "w");
    EXPECT_DOUBLE_EQ (boxes[0].second.lo[0], -1);
    EXPECT_DOUBLE_EQ (boxes[0].second.lo[1], 0);
    EXPECT_DOUBLE_EQ (boxes[0].second.lo[2], 0);
    EXPECT_DOUBLE_EQ (boxes[0].second.hi[0], 4);
    EXPECT_DOUBLE_EQ (boxes[0].second.hi[1], 5);
    EXPECT_DOUBLE_EQ (boxes[0].second.hi[2], 7);
}

// The 3 s generator lands while the user is idle, and rarely.
TEST (ModelStampWalk, TheBackstopWaitsForAStillProjectAndItsSpacing)
{
    EXPECT_FALSE (walk::Backstop3DDue (false, 999999, 999999)) << "nothing moved";
    EXPECT_FALSE (walk::Backstop3DDue (true, walk::kBackstopIdleMs - 1, 999999)) << "still working";
    EXPECT_FALSE (walk::Backstop3DDue (true, 999999, walk::kBackstopSpacingMs - 1)) << "ran too recently";
    EXPECT_TRUE (walk::Backstop3DDue (true, walk::kBackstopIdleMs, walk::kBackstopSpacingMs));
}
