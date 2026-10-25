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

TEST (EditGestureReplay, APreviewStepKeepsTheSurfaceNearItsTriangleBudget)
{
    namespace ed = geomsrv::edit;
    double last = 0.0;
    for (int n : { 3, 5, 8 }) {
        const auto scene = er::Grid ("g", n);
        const double step = ed::PreviewStep (scene.doc.paths, 3000, 0.5, 50.0);
        EXPECT_GE (step, last) << n; // a larger network is never paved finer
        last = step;
        EXPECT_EQ (std::fmod (step, 0.5), 0.0) << step;
        const auto preview = ed::Pave (scene.doc.paths, step, true);
        const size_t triangles = preview.triangles.size () / 3;
        EXPECT_LT (triangles, 3000u * 2) << n << " paths at " << step << " m";
        EXPECT_GT (triangles, 3000u / 8) << n << " paths at " << step << " m";
        // Every authored grade break is still a vertex of the preview, at the height the resting
        // surface gives it (a junction blends both the same way).
        const auto rest = ed::Pave (scene.doc.paths, 0.5, true);
        for (const auto& path : scene.doc.paths)
            for (const auto& b : path.profile) {
                ed::AlignmentGeometry g;
                std::string error;
                ASSERT_TRUE (ed::ValidatePath (path, g, error)) << error;
                ed::StationFrame f;
                ASSERT_TRUE (ed::Evaluate (path, g, b.station, f));
                const auto at = [&] (const ed::PavedArea& area, double& z) {
                    for (const auto& v : area.vertices)
                        if (ed::DistanceXY (v, f.centre) < 1e-6) {
                            z = v.z;
                            return true;
                        }
                    return false;
                };
                double coarse = 0, fine = 0;
                ASSERT_TRUE (at (preview, coarse)) << "path " << path.id << " break at " << b.station;
                ASSERT_TRUE (at (rest, fine)) << "path " << path.id << " break at " << b.station;
                EXPECT_EQ (coarse, fine) << "path " << path.id << " break at " << b.station;
            }
    }
    const auto small = er::Grid ("g", 2);
    EXPECT_EQ (ed::PreviewStep (small.doc.paths, 3000, 2.0), 2.0); // never finer than asked
    EXPECT_EQ (ed::PreviewStep (er::Grid ("g", 8).doc.paths, 10, 0.5, 6.0), 6.0);
    EXPECT_EQ (ed::PreviewStep ({}, 3000, 0.75), 0.75);
    // A drag that lengthens a path a little keeps the step.
    auto doc = er::Grid ("g", 5).doc;
    const double before = ed::PreviewStep (doc.paths);
    std::string error;
    ASSERT_TRUE (ed::MovePathPoint (doc, doc.paths[0].id, doc.paths[0].points[0].id, { 0, -5.3, 0 }, error)) << error;
    EXPECT_EQ (ed::PreviewStep (doc.paths), before);
}
