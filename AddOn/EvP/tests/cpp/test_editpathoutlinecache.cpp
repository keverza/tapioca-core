#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditPathOutlineCache.hpp"
#include "Edit/EditSnap.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;

namespace {
bool SameOutline (const ed::PathOutline& a, const ed::PathOutline& b)
{
    return a.stations == b.stations && a.centre == b.centre && a.left == b.left && a.right == b.right &&
           a.range == b.range && a.points == b.points && a.breaks == b.breaks;
}
} // namespace

TEST (EditPathOutlineCache, AnUnchangedPathIsTessellatedOncePerStep)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 8, 1 } }, 2.0);
    ed::PathOutlineCache cache;
    const auto* a = cache.Get (doc.paths[0], 0.5);
    ASSERT_NE (a, nullptr);
    ed::PathOutline plain;
    std::string error;
    ASSERT_TRUE (ed::Outline (doc.paths[0], 0.5, plain, error)) << error;
    EXPECT_TRUE (SameOutline (*a, plain));
    EXPECT_EQ (cache.Get (doc.paths[0], 0.5), a);
    EXPECT_NE (cache.Get (doc.paths[0], 2.0), nullptr);
    EXPECT_EQ (cache.builds, 2u);
    EXPECT_EQ (cache.hits, 1u);
    // The same id with another value is rebuilt, never served stale.
    ASSERT_TRUE (ed::MovePathPoint (doc, id, doc.paths[0].points[0].id, { 0, 3, 0 }, error)) << error;
    const auto* moved = cache.Get (doc.paths[0], 0.5);
    ASSERT_NE (moved, nullptr);
    ASSERT_TRUE (ed::Outline (doc.paths[0], 0.5, plain, error)) << error;
    EXPECT_TRUE (SameOutline (*moved, plain));
    EXPECT_EQ (cache.builds, 3u);
    EXPECT_EQ (cache.Size (), 2u); // it replaced its own 0.5 m entry
}

TEST (EditPathOutlineCache, PruneKeepsWhatTheFrameUsedAndBoundsEachPath)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 0, 5, 0 }, { 10, 5, 0 } }, 2.0);
    ed::PathOutlineCache cache;
    cache.Get (doc.paths[0], 0.5);
    cache.Get (doc.paths[1], 0.5);
    cache.Prune ();
    EXPECT_EQ (cache.Size (), 2u);
    cache.Get (doc.paths[0], 0.5); // the second path was deleted
    cache.Prune ();
    EXPECT_EQ (cache.Size (), 1u);
    for (double step : { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 })
        cache.Get (doc.paths[0], step);
    EXPECT_LE (cache.Size (), 4u);
    ed::Path broken = doc.paths[0];
    broken.sections.clear ();
    EXPECT_EQ (cache.Get (broken, 0.5), nullptr);
    EXPECT_EQ (cache.Get (broken, 0.5), nullptr); // its failure is cached too
}

TEST (EditPathOutlineCache, SnapAndPaveAreIdenticalWithTheCache)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 2.0);
    ed::AddPath (doc, { { 10, -10, 0 }, { 10, 10, 1 } }, 3.0);
    ed::PathOutlineCache cache;
    for (int pass = 0; pass < 2; ++pass) { // a cold and a warm cache
        for (bool surface : { false, true }) {
            const auto plain = ed::Pave (doc.paths, 0.5, surface), cached = ed::Pave (doc.paths, 0.5, surface, &cache);
            EXPECT_EQ (plain.rings, cached.rings);
            EXPECT_EQ (plain.vertices, cached.vertices);
            EXPECT_EQ (plain.triangles, cached.triangles);
            EXPECT_EQ (plain.corners.size (), cached.corners.size ());
        }
        ed::SnapQuery q;
        q.cursor = { 10.9, 0.95, 0 };
        q.tolerance = 0.3;
        q.pathStep = 0.5;
        const auto plain = ed::Snap (doc, {}, q);
        q.outlines = &cache;
        const auto cached = ed::Snap (doc, {}, q);
        EXPECT_EQ (plain.snapped, cached.snapped);
        EXPECT_EQ (plain.kind, cached.kind);
        EXPECT_EQ (plain.point, cached.point);
        EXPECT_EQ (plain.source, cached.source);
    }
    EXPECT_EQ (cache.builds, 2u); // two paths at one step, shared by the snap and both pavings
}
