#include "edit_gesture_replay.hpp"
#include <gtest/gtest.h>

namespace er = editreplay;

TEST (EditGestureReplay, ScenesAreSeededAndValid)
{
    const auto a = er::Grid ("small", 3), b = er::Grid ("small", 3);
    EXPECT_EQ (a.doc.paths, b.doc.paths);
    EXPECT_EQ (a.doc.entities, b.doc.entities);
    EXPECT_EQ (a.paths.size (), 6u);
    std::string error;
    EXPECT_TRUE (geomsrv::edit::Validate (a.doc, error)) << error;
}

TEST (EditGestureReplay, EveryGestureDoesRealWorkEachFrame)
{
    const auto scene = er::Grid ("small", 3);
    for (auto g : { er::Gesture::Hover, er::Gesture::Drag, er::Gesture::Width }) {
        std::vector<er::FrameResult> results;
        const auto t = er::Replay (scene, g, 12, {}, {}, &results);
        ASSERT_EQ (t.ms.size (), 12u);
        ASSERT_EQ (results.size (), 12u);
        size_t snapped = 0, hits = 0, edited = 0;
        for (const auto& r : results) {
            snapped += r.snapped;
            hits += r.hits.size ();
            edited += r.edited;
            if (g != er::Gesture::Hover)
                EXPECT_GT (r.triangles, 0u) << er::GestureName (g);
        }
        EXPECT_GT (snapped, 0u) << er::GestureName (g);
        if (g == er::Gesture::Hover)
            EXPECT_GT (hits, 0u);
        else
            EXPECT_EQ (edited, 12u) << er::GestureName (g);
    }
}

// The determinism gate every frame cache is held to: identical results with and without it.
TEST (EditGestureReplay, CachedFramesMatchUncachedFrames)
{
    const auto scene = er::Grid ("small", 4);
    for (auto g : { er::Gesture::Hover, er::Gesture::Drag, er::Gesture::Width }) {
        std::vector<er::FrameResult> plain, cached;
        er::Replay (scene, g, 16, {}, {}, &plain);
        geomsrv::edit::PickGeometryCache pick;
        er::Caches caches;
        caches.pick = &pick;
        er::Replay (scene, g, 16, {}, caches, &cached);
        ASSERT_EQ (plain.size (), cached.size ());
        for (size_t i = 0; i < plain.size (); ++i)
            EXPECT_TRUE (plain[i] == cached[i]) << er::GestureName (g) << " frame " << i;
    }
}

TEST (EditGestureReplay, PercentilesAreOrderStatistics)
{
    er::Timing t;
    for (int i = 1; i <= 100; ++i)
        t.ms.push_back (101 - i);
    EXPECT_DOUBLE_EQ (t.Percentile (0.5), 50);
    EXPECT_DOUBLE_EQ (t.Percentile (0.95), 95);
    EXPECT_DOUBLE_EQ (t.Percentile (1.0), 100);
}
