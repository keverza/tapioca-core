#include "Preview/RetainedPreviewStore.hpp"
#include "Annotation/RetainedTraceSelection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

namespace {

namespace preview = evp::preview;

TEST (RetainedPreviewStore, PublishesImmutableSnapshotsWithOneGenerationSequence)
{
    preview::RetainedPreviewStore store;

    preview::PreviewScene scene;
    scene.kind = "plan2d";
    scene.notes.push_back ("first");
    const uint64_t previewGeneration = store.PublishPreviewScene (std::move (scene));
    const auto firstPreview = store.PreviewSceneSnapshotCopy ();

    ASSERT_NE (firstPreview, nullptr);
    EXPECT_EQ (firstPreview->generation, previewGeneration);
    EXPECT_EQ (firstPreview->scene.notes, std::vector<std::string> ({ "first" }));

    preview::WatchPrimitive primitive;
    primitive.kind = preview::WatchPrimitiveKind::Arrow;
    primitive.points = { 0.0, 0.0, 0.0, 1.0, 2.0, 3.0 };
    preview::WatchFrame frame;
    frame.primitives.push_back (primitive);
    preview::WatchNode node;
    node.name = "direction";
    node.frames.push_back (frame);
    preview::WatchTrace trace;
    trace.nodes.push_back (node);

    const uint64_t traceGeneration = store.PublishWatchTrace (std::move (trace));
    const auto retainedTrace = store.WatchTraceSnapshotCopy ();

    ASSERT_NE (retainedTrace, nullptr);
    EXPECT_GT (traceGeneration, previewGeneration);
    EXPECT_EQ (retainedTrace->generation, traceGeneration);
    EXPECT_EQ (retainedTrace->trace.nodes[0].frames[0].primitives[0].points.size (), 6u);
    EXPECT_EQ (firstPreview->scene.notes, std::vector<std::string> ({ "first" }));
}

TEST (RetainedPreviewStore, ReturnsNullBeforeEachPayloadIsPublished)
{
    preview::RetainedPreviewStore store;

    EXPECT_EQ (store.PreviewSceneSnapshotCopy (), nullptr);
    EXPECT_EQ (store.WatchTraceSnapshotCopy (), nullptr);
}

TEST (RetainedPreviewStore, SelectsFirstAvailableFrameAndPublishesSelectionAtomically)
{
    preview::RetainedPreviewStore store;
    preview::WatchTrace trace;
    trace.nodes.resize (2);
    trace.nodes[1].name = "steps";
    trace.nodes[1].frames.resize (2);
    trace.nodes[1].frames[1].index = 7;

    store.PublishWatchTrace (std::move (trace));
    auto selected = store.SelectedWatchFrameSnapshotCopy ();
    ASSERT_TRUE (selected.has_value ());
    EXPECT_EQ (selected->nodeIndex, 1u);
    EXPECT_EQ (selected->frameIndex, 0u);

    ASSERT_TRUE (store.SelectWatchFrame (1, 1));
    selected = store.SelectedWatchFrameSnapshotCopy ();
    ASSERT_TRUE (selected.has_value ());
    EXPECT_EQ (selected->Frame ().index, 7u);
    EXPECT_FALSE (store.SelectWatchFrame (0, 0));
    EXPECT_EQ (store.SelectedWatchFrameSnapshotCopy ()->Frame ().index, 7u);

    store.ClearWatchSelection ();
    EXPECT_FALSE (store.SelectedWatchFrameSnapshotCopy ().has_value ());
}

TEST (RetainedPreviewStore, SerializesConcurrentPublishers)
{
    preview::RetainedPreviewStore store;
    constexpr int publishesPerThread = 100;

    std::thread previewPublisher ([&store] {
        for (int index = 0; index < publishesPerThread; ++index)
            store.PublishPreviewScene (preview::PreviewScene {});
    });
    std::thread tracePublisher ([&store] {
        for (int index = 0; index < publishesPerThread; ++index)
            store.PublishWatchTrace (preview::WatchTrace {});
    });
    previewPublisher.join ();
    tracePublisher.join ();

    const auto scene = store.PreviewSceneSnapshotCopy ();
    const auto trace = store.WatchTraceSnapshotCopy ();
    ASSERT_NE (scene, nullptr);
    ASSERT_NE (trace, nullptr);
    EXPECT_EQ (std::max (scene->generation, trace->generation), 2u * publishesPerThread);
}

TEST (RetainedPreviewStore, RetainedAnnotationSelectionFollowsTheSelectedWatchFrame)
{
    preview::WatchPrimitive primitive;
    primitive.kind = preview::WatchPrimitiveKind::Point;
    primitive.points = { 1.0, 2.0, 3.0 };
    preview::WatchTrace trace;
    trace.nodes.resize (1);
    trace.nodes[0].name = "steps";
    trace.nodes[0].frames.resize (2);
    trace.nodes[0].frames[1].index = 9;
    trace.nodes[0].frames[1].primitives.push_back (primitive);

    auto& store = preview::RetainedPreviewStore::Get ();
    store.PublishWatchTrace (std::move (trace));
    ASSERT_TRUE (store.SelectWatchFrame (0, 1));

    const auto selected = geomsrv::annotation::SelectedRetainedFrameSnapshotCopy ();
    ASSERT_TRUE (selected.has_value ());
    EXPECT_EQ (selected->SelectedFrame ().index, 9u);
    ASSERT_EQ (selected->SelectedFrame ().primitives.size (), 1u);
    EXPECT_DOUBLE_EQ (selected->SelectedFrame ().primitives[0].points[0].y, 2.0);
}

} // namespace
