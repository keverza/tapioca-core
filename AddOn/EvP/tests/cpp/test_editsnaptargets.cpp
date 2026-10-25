#include "edit_gesture_replay.hpp"
#include "Edit/EditSnap.hpp"
#include <gtest/gtest.h>
#include <cstdint>

namespace ed = geomsrv::edit;

namespace {
// A small seeded generator, so a failure names its query.
struct Lcg {
    uint64_t s;
    double Next (double lo, double hi)
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return lo + (hi - lo) * double (s >> 11) / double (1ull << 53);
    }
};

void ExpectSame (const ed::SnapResult& a, const ed::SnapResult& b, int i)
{
    EXPECT_EQ (a.snapped, b.snapped) << i;
    EXPECT_EQ (a.kind, b.kind) << i;
    EXPECT_EQ (a.point, b.point) << i;
    EXPECT_EQ (a.source, b.source) << i;
    EXPECT_EQ (a.contextSource, b.contextSource) << i;
    EXPECT_EQ (a.hint, b.hint) << i;
    EXPECT_EQ (a.onEdge, b.onEdge) << i;
    EXPECT_EQ (a.edgeFrom, b.edgeFrom) << i;
    EXPECT_EQ (a.edgeTo, b.edgeTo) << i;
    ASSERT_EQ (a.guides.size (), b.guides.size ()) << i;
    for (size_t g = 0; g < a.guides.size (); ++g) {
        EXPECT_EQ (a.guides[g].from, b.guides[g].from) << i;
        EXPECT_EQ (a.guides[g].to, b.guides[g].to) << i;
        EXPECT_EQ (a.guides[g].kind, b.guides[g].kind) << i;
    }
}
} // namespace

TEST (EditSnapTargets, IndexedSnapsAreTheFullScansOnSeededQueries)
{
    auto scene = editreplay::Grid ("g", 4);
    ed::SnapContext context;
    context.points.push_back ({ { 12.5, 7.25, 0 }, "door" });
    context.segments.push_back ({ { -4, 30, 0 }, { 70, 31, 0 }, "wall" });
    const ed::Id moved = scene.paths.front ();
    const auto& firstPoint = ed::FindPath (scene.doc, moved)->points.front ();
    ed::SnapTargets targets;
    Lcg rng { 7 };
    int snapped = 0;
    for (int i = 0; i < 3000; ++i) {
        ed::SnapQuery q;
        q.cursor = { rng.Next (-8, 70), rng.Next (-8, 70), 0 };
        q.tolerance = rng.Next (0.02, 1.5);
        q.pathStep = 0.5;
        if (i % 3 != 0)
            q.anchor = ed::Vec3 { rng.Next (-8, 70), rng.Next (-8, 70), 0 };
        if (i % 5 == 0)
            q.references.push_back ({ { 0, 0, 0 }, { 1, 2, 0 } });
        if (i % 7 == 0)
            q.exclude.push_back ({ moved, ed::Part::PathPoint, firstPoint.id });
        if (i % 11 == 0)
            q.extensionReach = rng.Next (0.5, 6.0);
        if (i % 13 == 0) { // a tilted working plane
            q.plane.normal = { 0.2, -0.1, 1.0 };
            q.plane.origin = { 3, 4, 0.5 };
            q.keepPlane = i % 2 == 0;
        }
        if (i % 17 == 0)
            q.modes = ed::kSnapEndpoint | ed::kSnapExtension;
        const ed::SnapResult plain = ed::Snap (scene.doc, context, q);
        targets.Update (scene.doc, context, 1, q);
        q.targets = &targets;
        const ed::SnapResult indexed = ed::Snap (scene.doc, context, q);
        snapped += plain.snapped;
        ExpectSame (plain, indexed, i);
        if (HasFailure ())
            return;
    }
    EXPECT_GT (snapped, 1000); // the queries do land on things
}

TEST (EditSnapTargets, ItRebuildsOnlyWhenWhatItGatheredChanges)
{
    auto scene = editreplay::Grid ("g", 3);
    ed::SnapContext context;
    ed::SnapTargets targets;
    ed::SnapQuery q;
    q.pathStep = 0.5;
    EXPECT_TRUE (targets.Update (scene.doc, context, 0, q));
    EXPECT_GT (targets.Segments (), 0u);
    for (int i = 0; i < 20; ++i) {
        q.cursor = { double (i), 1, 0 }; // the pointer moves: nothing to gather again
        EXPECT_FALSE (targets.Update (scene.doc, context, 0, q));
    }
    EXPECT_EQ (targets.builds, 1u);
    EXPECT_TRUE (targets.Update (scene.doc, context, 1, q)); // the host context changed
    q.exclude.push_back ({ scene.paths[0], ed::Part::Entity, 0 });
    EXPECT_TRUE (targets.Update (scene.doc, context, 1, q));
    q.plane.origin.z = 1.0;
    EXPECT_TRUE (targets.Update (scene.doc, context, 1, q));
    std::string error;
    ASSERT_TRUE (ed::SetPathWidth (scene.doc, scene.paths[1], scene.doc.paths[1].sections[0].id, 3, 3, error));
    EXPECT_TRUE (targets.Update (scene.doc, context, 1, q)); // a new revision
    EXPECT_EQ (targets.builds, 5u);
    // Built for another query, it is not used: the snap gathers as before.
    ed::SnapQuery other = q;
    other.pathStep = 2.0;
    other.cursor = { 0.1, 0.1, 0 };
    other.tolerance = 0.5;
    const auto plain = ed::Snap (scene.doc, context, other);
    other.targets = &targets;
    ExpectSame (plain, ed::Snap (scene.doc, context, other), 0);
    targets.Reset ();
    EXPECT_EQ (targets.Points (), 0u);
}
