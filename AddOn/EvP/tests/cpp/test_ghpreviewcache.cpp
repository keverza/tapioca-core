// Preview/GhPreviewCache.cpp — the host's mirror of what Grasshopper previews.
//
// Everything this file asserts fails SILENTLY in Archicad, which is why it is
// tested rather than looked at. A half-applied batch is a picture of a building
// that never existed and nothing in it says so. A stale-epoch batch draws
// geometry from a process that has been killed. A missed checksum leaves a
// viewport permanently disagreeing with the canvas, and it stays wrong until
// someone reopens the definition. None of the three produces an error, a log
// line, or anything on screen a user could report as a fault.
//
// The delta semantics are the other half: identity that survives an edit,
// visibility that costs a byte, selection that moves no vertices. Getting those
// wrong does not break the picture — it makes a slider drag retransmit the whole
// definition, and the symptom is "Grasshopper preview is slow", which is the
// hardest kind of bug to trace back to a cache rule.

#include "Preview/GhPreviewCache.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp::preview;
using namespace evp::grasshopper::protocol;

namespace {

PreviewPrimitiveMessage Line (uint64_t id, uint32_t revision, float shift)
{
    PreviewPrimitiveMessage message;
    message.header.primitiveId = id;
    message.header.kind = PreviewKind::Polyline3D;
    message.header.flags = PreviewFlagVisible | PreviewFlagDepthTest;
    message.header.revision = revision;
    message.header.contentHash = (uint64_t) (1000 + (int) shift);
    message.descriptor.positionFloats = 6;
    message.positions = { shift, 0, 0, shift + 1, 0, 0 };
    return message;
}

PreviewBeginBatchPayload Begin (uint32_t epoch, uint32_t revision, uint32_t count)
{
    PreviewBeginBatchPayload begin;
    begin.epoch = epoch;
    begin.revision = revision;
    begin.primitiveCount = count;
    return begin;
}

PreviewIdRunPayload IdRun (uint32_t epoch, uint32_t revision, std::vector<uint64_t> ids)
{
    PreviewIdRunPayload run;
    run.epoch = epoch;
    run.revision = revision;
    run.ids = std::move (ids);
    return run;
}

// The worker computes the footer from what it sent; a test that recomputed it
// from what the CACHE accumulated would prove nothing, so this walks the same
// (id, change) sequence the batch was fed.
struct Footer {
    uint64_t checksum = PreviewChecksumStart ();
    uint32_t entries = 0;

    void Add (uint64_t id, PreviewChange change)
    {
        checksum = PreviewChecksumAccumulate (checksum, id, change);
        ++entries;
    }

    PreviewEndBatchPayload End (uint32_t epoch, uint32_t revision) const
    {
        PreviewEndBatchPayload end;
        end.epoch = epoch;
        end.revision = revision;
        end.entryCount = entries;
        end.checksum = checksum;
        return end;
    }
};

const GhPreviewPrimitive* Find (const std::shared_ptr<const GhPreviewSnapshot>& snapshot, uint64_t id)
{
    if (!snapshot)
        return nullptr;
    for (const auto& primitive : snapshot->primitives) {
        if (primitive->id == id)
            return primitive.get ();
    }
    return nullptr;
}

} // namespace

TEST (GhPreviewCache, ABatchIsAppliedWholeAndPublishedOnce)
{
    GhPreviewCache cache;
    std::string reason;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 2), reason)) << reason;

    Footer footer;
    ASSERT_EQ (GhPreviewApply::Applied, cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason)) << reason;
    footer.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.Apply (Line (20, 1, 5), PreviewChange::Added, reason)) << reason;
    footer.Add (20, PreviewChange::Added);

    // ⚠️ NOTHING IS VISIBLE UNTIL EndBatch. Half a batch on screen is the whole
    // reason staging exists.
    EXPECT_EQ ((size_t) 0, cache.Count ());

    const GhPreviewEndResult result = cache.EndBatch (footer.End (1, 1));
    ASSERT_EQ (GhPreviewApply::Applied, result.apply) << result.reason;
    EXPECT_FALSE (result.resyncRequired);
    EXPECT_EQ ((size_t) 2, cache.Count ());

    auto snapshot = cache.SnapshotCopy ();
    ASSERT_TRUE (snapshot != nullptr);
    EXPECT_EQ (1u, snapshot->revision);
    EXPECT_EQ ((size_t) 2, snapshot->primitives.size ());
}

TEST (GhPreviewCache, AnAbandonedBatchLeavesTheLastCompletePreviewStanding)
{
    // What a worker death mid-batch means. The viewport keeps showing the last
    // picture that was ever true, rather than a partial one that never was.
    GhPreviewCache cache;
    std::string reason;
    Footer first;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);
    first.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (first.End (1, 1)).apply);

    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 2, 1), reason));
    cache.Apply (Line (20, 2, 9), PreviewChange::Added, reason);
    cache.AbandonBatch ("the worker died");

    EXPECT_FALSE (cache.BatchOpen ());
    EXPECT_EQ ((size_t) 1, cache.Count ());
    EXPECT_TRUE (Find (cache.SnapshotCopy (), 10) != nullptr);
    EXPECT_TRUE (Find (cache.SnapshotCopy (), 20) == nullptr);
}

TEST (GhPreviewCache, AChecksumDisagreementAsksForAResyncInsteadOfDrawing)
{
    // Cheap insurance against a dropped or reordered message becoming a
    // permanently wrong viewport: the alternative is trusting arrival order,
    // which fails silently and stays failed.
    GhPreviewCache cache;
    std::string reason;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);

    Footer wrong;
    wrong.Add (11, PreviewChange::Added); // as if a different id had been sent

    const GhPreviewEndResult result = cache.EndBatch (wrong.End (1, 1));
    EXPECT_EQ (GhPreviewApply::Refused, result.apply);
    EXPECT_TRUE (result.resyncRequired);
    EXPECT_EQ ((size_t) 0, cache.Count ());
    EXPECT_FALSE (cache.BatchOpen ());
}

TEST (GhPreviewCache, AnEntryCountDisagreementAsksForAResyncToo)
{
    GhPreviewCache cache;
    std::string reason;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);

    Footer footer;
    footer.Add (10, PreviewChange::Added);
    PreviewEndBatchPayload end = footer.End (1, 1);
    end.entryCount = 7;

    const GhPreviewEndResult result = cache.EndBatch (end);
    EXPECT_EQ (GhPreviewApply::Refused, result.apply);
    EXPECT_TRUE (result.resyncRequired);
}

TEST (GhPreviewCache, AStaleEpochIsDroppedWithoutComplaint)
{
    // After a kill and restart the host holds preview from a process that no
    // longer exists. This is not an error; it is what a restart looks like from
    // here, and reporting it as one would fill the log after every recovery.
    GhPreviewCache cache;
    std::string reason;
    Footer footer;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (4, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);
    footer.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (footer.End (4, 1)).apply);

    EXPECT_EQ (GhPreviewApply::Ignored, cache.BeginBatch (Begin (3, 2, 1), reason));
    EXPECT_EQ (GhPreviewApply::Ignored, cache.SetFlags (IdRun (3, 2, { 10 }), reason));
    EXPECT_EQ ((size_t) 1, cache.Count ());
}

TEST (GhPreviewCache, DropAllForgetsEverythingAndAdoptsTheNewGeneration)
{
    GhPreviewCache cache;
    std::string reason;
    Footer footer;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);
    footer.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (footer.End (1, 1)).apply);

    cache.DropAll (2, "the worker restarted");
    EXPECT_EQ ((size_t) 0, cache.Count ());
    EXPECT_EQ (2u, cache.Epoch ());

    // The new generation starts its revisions again, and that must not be read
    // as a replay of the old ones.
    Footer fresh;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (2, 1, 1), reason)) << reason;
    cache.Apply (Line (30, 1, 0), PreviewChange::Added, reason);
    fresh.Add (30, PreviewChange::Added);
    EXPECT_EQ (GhPreviewApply::Applied, cache.EndBatch (fresh.End (2, 1)).apply);
}

TEST (GhPreviewCache, ARepeatedRevisionIsRefusedWithinOneEpoch)
{
    GhPreviewCache cache;
    std::string reason;
    Footer footer;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 5, 1), reason));
    cache.Apply (Line (10, 5, 0), PreviewChange::Added, reason);
    footer.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (footer.End (1, 5)).apply);

    EXPECT_EQ (GhPreviewApply::Refused, cache.BeginBatch (Begin (1, 5, 1), reason));
    EXPECT_EQ (GhPreviewApply::Refused, cache.BeginBatch (Begin (1, 4, 1), reason));
}

TEST (GhPreviewCache, ASecondOpenBatchIsRefused)
{
    GhPreviewCache cache;
    std::string reason;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    EXPECT_EQ (GhPreviewApply::Refused, cache.BeginBatch (Begin (1, 2, 1), reason));
}

TEST (GhPreviewCache, IdentitySurvivesAContentChange)
{
    // ⚠️ THE WHOLE DELTA PROTOCOL RESTS ON THIS. If a changed primitive arrived
    // under a new id it would read as a Remove plus an Add, a slider drag would
    // retransmit the definition, and selection would be lost on every solve.
    GhPreviewCache cache;
    std::string reason;
    Footer first;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);
    first.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (first.End (1, 1)).apply);

    Footer second;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 2, 1), reason));
    cache.Apply (Line (10, 2, 40), PreviewChange::Changed, reason);
    second.Add (10, PreviewChange::Changed);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (second.End (1, 2)).apply);

    EXPECT_EQ ((size_t) 1, cache.Count ());
    const GhPreviewPrimitive* primitive = Find (cache.SnapshotCopy (), 10);
    ASSERT_TRUE (primitive != nullptr);
    EXPECT_FLOAT_EQ (40.0f, primitive->positions[0]);
    EXPECT_EQ (2u, primitive->revision);
}

TEST (GhPreviewCache, PreviewOffIsAVisibilityFlagAndLeavesTheGeometryAlone)
{
    // Toggling a component's preview back on must cost a byte, not a
    // retransmission, so a hidden primitive stays in the cache with its buffers.
    GhPreviewCache cache;
    std::string reason;
    Footer first;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 3), PreviewChange::Added, reason);
    first.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (first.End (1, 1)).apply);

    PreviewIdRunPayload run = IdRun (1, 1, { 10 });
    run.flagMask = PreviewFlagVisible;
    run.flagValue = 0;
    ASSERT_EQ (GhPreviewApply::Applied, cache.SetFlags (run, reason)) << reason;

    const GhPreviewPrimitive* primitive = Find (cache.SnapshotCopy (), 10);
    ASSERT_TRUE (primitive != nullptr);
    EXPECT_FALSE (primitive->Visible ());
    EXPECT_EQ ((size_t) 6, primitive->positions.size ());
    EXPECT_FLOAT_EQ (3.0f, primitive->positions[0]);
    // The other flags are untouched: a visibility message speaks for visibility.
    EXPECT_NE (0u, primitive->flags & PreviewFlagDepthTest);
}

TEST (GhPreviewCache, SelectionArrivesBetweenBatchesAndMovesNoVertices)
{
    // Clicking a component on the canvas does not re-solve, so there is no batch
    // to carry the selection. It applies to the live cache and publishes on its
    // own — and it must never touch geometry, which is what makes a selection
    // change over hundreds of primitives cost bytes rather than megabytes.
    GhPreviewCache cache;
    std::string reason;
    Footer footer;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);
    footer.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (footer.End (1, 1)).apply);

    auto before = cache.SnapshotCopy ();
    const std::vector<float> geometry = Find (before, 10)->positions;

    PreviewIdRunPayload run = IdRun (1, 1, { 10 });
    run.flagMask = PreviewFlagSelected;
    run.flagValue = PreviewFlagSelected;
    ASSERT_FALSE (cache.BatchOpen ());
    ASSERT_EQ (GhPreviewApply::Applied, cache.SetFlags (run, reason)) << reason;

    auto after = cache.SnapshotCopy ();
    ASSERT_TRUE (after != before);
    const GhPreviewPrimitive* primitive = Find (after, 10);
    ASSERT_TRUE (primitive != nullptr);
    EXPECT_TRUE (primitive->Selected ());
    EXPECT_EQ (geometry, primitive->positions);
}

TEST (GhPreviewCache, AFlagChangeForAnIdTheHostDoesNotHoldIsQuietlySkipped)
{
    // The two mirrors can legitimately disagree by one batch after a resync.
    // Refusing here would turn a harmless disagreement into a dropped message.
    GhPreviewCache cache;
    std::string reason;
    PreviewIdRunPayload run = IdRun (0, 0, { 999 });
    run.flagMask = PreviewFlagSelected;
    run.flagValue = PreviewFlagSelected;
    EXPECT_EQ (GhPreviewApply::Applied, cache.SetFlags (run, reason));
}

TEST (GhPreviewCache, RemovalTakesAnIdAndNothingElse)
{
    GhPreviewCache cache;
    std::string reason;
    Footer first;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 2), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);
    first.Add (10, PreviewChange::Added);
    cache.Apply (Line (20, 1, 5), PreviewChange::Added, reason);
    first.Add (20, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (first.End (1, 1)).apply);

    Footer second;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 2, 0), reason));
    ASSERT_EQ (GhPreviewApply::Applied, cache.Remove (IdRun (1, 2, { 20 }), reason)) << reason;
    second.Add (20, PreviewChange::Removed);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (second.End (1, 2)).apply);

    EXPECT_EQ ((size_t) 1, cache.Count ());
    EXPECT_TRUE (Find (cache.SnapshotCopy (), 20) == nullptr);
}

TEST (GhPreviewCache, APrimitiveOutsideABatchIsRefused)
{
    GhPreviewCache cache;
    std::string reason;
    EXPECT_EQ (GhPreviewApply::Refused, cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason));
    EXPECT_EQ (GhPreviewApply::Refused, cache.Remove (IdRun (0, 0, { 10 }), reason));
    EXPECT_EQ (GhPreviewApply::Refused, cache.EndBatch (PreviewEndBatchPayload ()).apply);
}

TEST (GhPreviewCache, APrimitiveFromAnotherRevisionInsideABatchIsRefused)
{
    GhPreviewCache cache;
    std::string reason;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 3, 1), reason));
    EXPECT_EQ (GhPreviewApply::Refused, cache.Apply (Line (10, 2, 0), PreviewChange::Added, reason));
}

TEST (GhPreviewCache, APrimitiveStillPointingIntoTheWorkersSegmentIsRefused)
{
    // ⚠️ THE CACHE MUST NEVER HOLD A REFERENCE INTO THE WORKER'S MEMORY. Reading
    // the segment happens BEFORE the ack that releases it; a producer that can
    // rewrite memory a render thread is reading is SceneCmdQueue's documented
    // bug, one process boundary further out.
    GhPreviewCache cache;
    std::string reason;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));

    PreviewPrimitiveMessage message = Line (10, 1, 0);
    message.inSegment = true;
    EXPECT_EQ (GhPreviewApply::Refused, cache.Apply (message, PreviewChange::Added, reason));
    EXPECT_NE (std::string::npos, reason.find ("segment"));
}

TEST (GhPreviewCache, AMalformedPrimitiveIsRefusedByTheCacheAsWellAsTheCodec)
{
    // Belt and braces on purpose: the cache is reachable from a decode path that
    // might one day skip a validator, and the cost of checking twice is a
    // comparison per primitive.
    GhPreviewCache cache;
    std::string reason;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));

    PreviewPrimitiveMessage message = Line (10, 1, 0);
    message.header.kind = PreviewKind::TriangleMesh;
    message.descriptor.indexCount = 3;
    message.indices = { 0, 1, 5 };
    EXPECT_EQ (GhPreviewApply::Refused, cache.Apply (message, PreviewChange::Added, reason));
}

TEST (GhPreviewCache, OneCacheHoldsBothSurfacesAndEachLayerAsksWhichIsItsOwn)
{
    // The cache is NOT split by surface on purpose: identity, selection, the
    // delta protocol and the pick range are the same problem in either window,
    // and two caches would duplicate all four. It is the LAYERS that split, and
    // DrawnOn is the question each one asks.
    GhPreviewCache cache;
    std::string reason;
    Footer footer;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 3), reason));

    PreviewPrimitiveMessage model = Line (10, 1, 0);
    model.header.surface = PreviewSurface::Model3D;
    cache.Apply (model, PreviewChange::Added, reason);
    footer.Add (10, PreviewChange::Added);

    PreviewPrimitiveMessage plan = Line (20, 1, 1);
    plan.header.surface = PreviewSurface::FloorPlan;
    cache.Apply (plan, PreviewChange::Added, reason);
    footer.Add (20, PreviewChange::Added);

    PreviewPrimitiveMessage both = Line (30, 1, 2);
    both.header.surface = PreviewSurface::Both;
    cache.Apply (both, PreviewChange::Added, reason);
    footer.Add (30, PreviewChange::Added);

    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (footer.End (1, 1)).apply);
    auto snapshot = cache.SnapshotCopy ();

    EXPECT_TRUE (Find (snapshot, 10)->DrawnOn (PreviewSurface::Model3D));
    EXPECT_FALSE (Find (snapshot, 10)->DrawnOn (PreviewSurface::FloorPlan));
    EXPECT_TRUE (Find (snapshot, 20)->DrawnOn (PreviewSurface::FloorPlan));
    EXPECT_FALSE (Find (snapshot, 20)->DrawnOn (PreviewSurface::Model3D));
    // Both answers yes to either, which is the whole reason it is a value rather
    // than the author wiring two components.
    EXPECT_TRUE (Find (snapshot, 30)->DrawnOn (PreviewSurface::Model3D));
    EXPECT_TRUE (Find (snapshot, 30)->DrawnOn (PreviewSurface::FloorPlan));
}

TEST (GhPreviewCache, RetargetingAComponentKeepsItsIdentityAndMovesIt)
{
    // An author who drags a definition's output from the 3D window to the floor
    // plan has changed what that component PRODUCES, not produced something
    // else. So it must diff as Changed under the same id -- keeping its
    // selection -- and the primitive must actually move surfaces.
    GhPreviewCache cache;
    std::string reason;
    Footer first;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    PreviewPrimitiveMessage model = Line (10, 1, 0);
    model.header.surface = PreviewSurface::Model3D;
    cache.Apply (model, PreviewChange::Added, reason);
    first.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (first.End (1, 1)).apply);

    Footer second;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 2, 1), reason));
    PreviewPrimitiveMessage plan = Line (10, 2, 0);
    plan.header.surface = PreviewSurface::FloorPlan;
    cache.Apply (plan, PreviewChange::Changed, reason);
    second.Add (10, PreviewChange::Changed);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (second.End (1, 2)).apply);

    EXPECT_EQ ((size_t) 1, cache.Count ());
    EXPECT_TRUE (Find (cache.SnapshotCopy (), 10)->DrawnOn (PreviewSurface::FloorPlan));
    EXPECT_FALSE (Find (cache.SnapshotCopy (), 10)->DrawnOn (PreviewSurface::Model3D));
}

TEST (GhPreviewCache, ASnapshotSurvivesTheNextBatch)
{
    // The render thread holds a shared_ptr across frames and never takes the
    // store lock; publishing must therefore never mutate what it is already
    // holding. This is the same contract RetainedPreviewStore makes.
    GhPreviewCache cache;
    std::string reason;
    Footer first;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 1), PreviewChange::Added, reason);
    first.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (first.End (1, 1)).apply);

    auto held = cache.SnapshotCopy ();
    ASSERT_TRUE (held != nullptr);
    const std::vector<float> geometry = Find (held, 10)->positions;

    Footer second;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 2, 1), reason));
    cache.Apply (Line (10, 2, 77), PreviewChange::Changed, reason);
    second.Add (10, PreviewChange::Changed);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (second.End (1, 2)).apply);

    EXPECT_EQ (geometry, Find (held, 10)->positions);
    EXPECT_FLOAT_EQ (77.0f, Find (cache.SnapshotCopy (), 10)->positions[0]);
    EXPECT_LT (held->generation, cache.SnapshotCopy ()->generation);
}

TEST (GhPreviewCache, SteadyStateCostsNothing)
{
    // "Nothing changed" is a budget line in the handoff: zero bytes. An empty
    // batch must still be a legal, cheap batch rather than something the cache
    // treats as a fault.
    GhPreviewCache cache;
    std::string reason;
    Footer first;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 1, 1), reason));
    cache.Apply (Line (10, 1, 0), PreviewChange::Added, reason);
    first.Add (10, PreviewChange::Added);
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (first.End (1, 1)).apply);

    Footer empty;
    ASSERT_EQ (GhPreviewApply::Applied, cache.BeginBatch (Begin (1, 2, 0), reason));
    ASSERT_EQ (GhPreviewApply::Applied, cache.EndBatch (empty.End (1, 2)).apply);
    EXPECT_EQ ((size_t) 1, cache.Count ());
    EXPECT_TRUE (Find (cache.SnapshotCopy (), 10) != nullptr);
}
