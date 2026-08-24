// ArchViz/SceneCmdQueue — the main -> render handover for geometry.
//
// ⚠️ WHY OFFLINE: this queue is where a lost, reordered or double-freed element
// would hide, and every one of those is invisible from inside Archicad — a lost
// element looks like a modelling mistake, a reordered batch looks like a
// half-loaded project. A debugger attached to Archicad is the worst possible
// place to find them, so the queue is deliberately bgfx-free and checked here.
//
// The ordering test is the one that matters most: the commands are a SEQUENCE,
// and an EndBatch that overtakes its upserts makes a full batch drop exactly the
// elements it was about to receive.

#include "ArchViz/SceneCmdQueue.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace geomsrv::archviz;

namespace {

std::unique_ptr<ElementUpload> MakeUpload (const std::string& guid, size_t verts = 3)
{
    auto up = std::make_unique<ElementUpload> ();
    up->guid = guid;
    up->vertices.assign (verts * 3, 1.0f);
    up->normals.assign (verts * 3, 0.0f);
    up->indices.assign (verts, 0u);
    up->ranges.push_back (MaterialRange { 0, 0, uint32_t (verts) });
    return up;
}

std::unique_ptr<PointLayerUpload> MakePointLayer (const std::string& layerId)
{
    auto layer = std::make_unique<PointLayerUpload> ();
    layer->layerId = layerId;
    layer->sourceId = "scan-17";
    layer->sourcePath = "clouds/site.ply";
    layer->rtcOrigin[0] = 6384123.25;
    layer->rtcOrigin[1] = 603123.5;
    layer->rtcOrigin[2] = 112.75;
    layer->boundsMin[0] = -10.0f;
    layer->boundsMax[0] = 20.0f;
    return layer;
}

std::unique_ptr<PointNodeUpload> MakePointNode (const std::string& layerId, uint32_t nodeId, size_t count = 4)
{
    auto node = std::make_unique<PointNodeUpload> ();
    node->layerId = layerId;
    node->nodeId = nodeId;
    node->parentId = 3;
    node->level = 2;
    node->boundsMin[0] = -2.0f;
    node->boundsMax[0] = 3.0f;
    node->geometricError = 0.125f;
    node->vertices.resize (count);
    node->vertices[0].position[0] = 1.25f;
    node->vertices[0].rgba = 0xFF332211u;
    return node;
}

// The queue is a singleton, so every test starts from a known state.
struct SceneQueueTest : ::testing::Test {
    void SetUp () override
    {
        SceneCmdQueue::Get ().Clear ();
    }
    void TearDown () override
    {
        SceneCmdQueue::Get ().Clear ();
    }
};

} // namespace

TEST_F (SceneQueueTest, PreservesOrderAcrossPartialTakes)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushBeginBatch (true);
    q.PushUpsert (MakeUpload ("a"));
    q.PushUpsert (MakeUpload ("b"));
    q.PushRemove ("c");
    q.PushEndBatch ();
    ASSERT_EQ (q.PendingCount (), 5u);

    // Taken in two bites, exactly as a frame-budgeted consumer would.
    std::vector<SceneCmd> first = q.Take (2);
    ASSERT_EQ (first.size (), 2u);
    EXPECT_EQ (first[0].type, SceneCmdType::BeginBatch);
    EXPECT_TRUE (first[0].full);
    ASSERT_NE (first[1].upload, nullptr);
    EXPECT_EQ (first[1].upload->guid, "a");

    std::vector<SceneCmd> rest = q.Take (99);
    ASSERT_EQ (rest.size (), 3u);
    ASSERT_NE (rest[0].upload, nullptr);
    EXPECT_EQ (rest[0].upload->guid, "b");
    EXPECT_EQ (rest[1].type, SceneCmdType::RemoveElement);
    EXPECT_EQ (rest[1].guid, "c");
    EXPECT_EQ (rest[2].type, SceneCmdType::EndBatch);

    EXPECT_EQ (q.PendingCount (), 0u);
    EXPECT_TRUE (q.Take (10).empty ());
}

// The consumer OWNS the payload once it takes it. If the queue kept a copy or
// freed it itself, this pointer would dangle — and under ASan that is a hard
// failure, which is the point of checking it here rather than in Archicad.
TEST_F (SceneQueueTest, ConsumerOwnsThePayload)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushUpsert (MakeUpload ("owned", 4));

    std::vector<SceneCmd> taken = q.Take (1);
    ASSERT_EQ (taken.size (), 1u);
    ASSERT_NE (taken[0].upload, nullptr);
    EXPECT_EQ (taken[0].upload->VertexCount (), 4u);
    EXPECT_EQ (taken[0].upload->vertices.size (), 12u);
    // Still alive and readable after the queue has moved on.
    q.Clear ();
    EXPECT_EQ (taken[0].upload->guid, "owned");
}

TEST_F (SceneQueueTest, PendingBytesRisesOnPushAndFallsOnTake)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    EXPECT_EQ (q.PendingBytes (), 0u);

    q.PushUpsert (MakeUpload ("a", 100));
    const size_t afterOne = q.PendingBytes ();
    EXPECT_GT (afterOne, 0u);

    q.PushUpsert (MakeUpload ("b", 100));
    EXPECT_GT (q.PendingBytes (), afterOne);

    // ⚠️ Bytes must drop when the payload LEAVES, not when the consumer
    // eventually frees it — a producer throttling on this would otherwise watch
    // a number that never falls and stall forever.
    q.Take (1);
    EXPECT_EQ (q.PendingBytes (), afterOne);
    q.Take (1);
    EXPECT_EQ (q.PendingBytes (), 0u);
}

// A non-upload command carries no payload; accounting must not underflow on it.
// PendingBytes is size_t, so an underflow would report ~1.8e19 bytes and any
// throttle reading it would stop the producer dead.
TEST_F (SceneQueueTest, NonUploadCommandsDoNotDisturbByteAccounting)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushBeginBatch (false);
    q.PushRemove ("gone");
    q.PushEndBatch ();
    EXPECT_EQ (q.PendingBytes (), 0u);
    q.Take (3);
    EXPECT_EQ (q.PendingBytes (), 0u);
}

TEST_F (SceneQueueTest, ClearDropsEverything)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushBeginBatch (true);
    q.PushUpsert (MakeUpload ("a", 50));
    q.Clear ();
    EXPECT_EQ (q.PendingCount (), 0u);
    EXPECT_EQ (q.PendingBytes (), 0u);
}

// A null upload must be ignored rather than queued: a null node would be a
// nullptr dereference on the consumer, i.e. a crash blamed on Archicad.
TEST_F (SceneQueueTest, NullUploadIsIgnored)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushUpsert (nullptr);
    EXPECT_EQ (q.PendingCount (), 0u);
}

TEST_F (SceneQueueTest, PointLayerCommandsPreserveMetadataAndOrderingOutsideFullBatch)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushBeginBatch (true);
    q.PushBeginPointLayer (MakePointLayer ("survey"));
    q.PushUpsertPointNode (MakePointNode ("survey", 7));
    q.PushEndBatch ();
    q.PushClearPointLayer ("old-survey");
    q.PushEndPointLayer ("survey");

    std::vector<SceneCmd> commands = q.Take (99);
    ASSERT_EQ (commands.size (), 6u);
    EXPECT_EQ (commands[0].type, SceneCmdType::BeginBatch);
    ASSERT_NE (commands[1].pointLayer, nullptr);
    EXPECT_EQ (commands[1].type, SceneCmdType::BeginPointLayer);
    EXPECT_EQ (commands[1].pointLayer->layerId, "survey");
    EXPECT_EQ (commands[1].pointLayer->sourceId, "scan-17");
    EXPECT_EQ (commands[1].pointLayer->sourcePath, "clouds/site.ply");
    EXPECT_DOUBLE_EQ (commands[1].pointLayer->rtcOrigin[0], 6384123.25);
    EXPECT_FLOAT_EQ (commands[1].pointLayer->boundsMin[0], -10.0f);
    EXPECT_FLOAT_EQ (commands[1].pointLayer->boundsMax[0], 20.0f);

    ASSERT_NE (commands[2].pointNode, nullptr);
    EXPECT_EQ (commands[2].type, SceneCmdType::UpsertPointNode);
    EXPECT_EQ (commands[2].pointNode->layerId, "survey");
    EXPECT_EQ (commands[2].pointNode->nodeId, 7u);
    EXPECT_EQ (commands[2].pointNode->parentId, 3u);
    EXPECT_EQ (commands[2].pointNode->level, 2u);
    EXPECT_FLOAT_EQ (commands[2].pointNode->boundsMin[0], -2.0f);
    EXPECT_FLOAT_EQ (commands[2].pointNode->boundsMax[0], 3.0f);
    EXPECT_FLOAT_EQ (commands[2].pointNode->geometricError, 0.125f);
    ASSERT_EQ (commands[2].pointNode->vertices.size (), 4u);
    EXPECT_FLOAT_EQ (commands[2].pointNode->vertices[0].position[0], 1.25f);
    EXPECT_EQ (commands[2].pointNode->vertices[0].rgba, 0xFF332211u);

    EXPECT_EQ (commands[3].type, SceneCmdType::EndBatch);
    EXPECT_EQ (commands[4].type, SceneCmdType::ClearPointLayer);
    EXPECT_EQ (commands[4].pointLayerId, "old-survey");
    EXPECT_EQ (commands[5].type, SceneCmdType::EndPointLayer);
    EXPECT_EQ (commands[5].pointLayerId, "survey");
}

TEST_F (SceneQueueTest, PointPayloadsAreOwnedAndIncludedInPendingBytes)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    std::unique_ptr<PointLayerUpload> layer = MakePointLayer ("survey");
    std::unique_ptr<PointNodeUpload> node = MakePointNode ("survey", 1, 100);
    const size_t layerBytes = layer->Bytes ();
    const size_t nodeBytes = node->Bytes ();

    q.PushBeginPointLayer (std::move (layer));
    q.PushUpsertPointNode (std::move (node));
    EXPECT_EQ (layer, nullptr);
    EXPECT_EQ (node, nullptr);
    EXPECT_EQ (q.PendingBytes (), layerBytes + nodeBytes);

    std::vector<SceneCmd> first = q.Take (1);
    ASSERT_NE (first[0].pointLayer, nullptr);
    EXPECT_EQ (q.PendingBytes (), nodeBytes);
    q.Clear ();
    EXPECT_EQ (first[0].pointLayer->sourceId, "scan-17");
    EXPECT_EQ (q.PendingBytes (), 0u);
}

TEST_F (SceneQueueTest, NullPointPayloadsAreIgnored)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushBeginPointLayer (nullptr);
    q.PushUpsertPointNode (nullptr);
    EXPECT_EQ (q.PendingCount (), 0u);
    EXPECT_EQ (q.PendingBytes (), 0u);
}

TEST_F (SceneQueueTest, TakeZeroTakesNothing)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.PushEndBatch ();
    EXPECT_TRUE (q.Take (0).empty ());
    EXPECT_EQ (q.PendingCount (), 1u);
}

// The real threading shape: one producer, one consumer, running at once. Not a
// proof of correctness under TSan, but it is what catches an unlocked accessor
// added later — and it runs under ASan in the sanitized build.
TEST_F (SceneQueueTest, SurvivesConcurrentProducerAndConsumer)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    constexpr int kCount = 2000;

    std::thread producer ([&q] {
        for (int i = 0; i < kCount; ++i)
            q.PushUpsert (MakeUpload ("g" + std::to_string (i), 3));
    });

    int seen = 0;
    while (seen < kCount) {
        for (SceneCmd& cmd : q.Take (16)) {
            ASSERT_NE (cmd.upload, nullptr);
            EXPECT_EQ (cmd.upload->guid, "g" + std::to_string (seen)); // order held
            ++seen;
        }
    }
    producer.join ();

    EXPECT_EQ (seen, kCount);
    EXPECT_EQ (q.PendingCount (), 0u);
    EXPECT_EQ (q.PendingBytes (), 0u);
}
