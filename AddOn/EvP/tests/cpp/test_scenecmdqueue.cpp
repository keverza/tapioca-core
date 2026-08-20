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

// The queue is a singleton, so every test starts from a known state.
struct SceneQueueTest : ::testing::Test {
    void SetUp () override    { SceneCmdQueue::Get ().Clear (); }
    void TearDown () override { SceneCmdQueue::Get ().Clear (); }
};

}   // namespace

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
            EXPECT_EQ (cmd.upload->guid, "g" + std::to_string (seen));   // order held
            ++seen;
        }
    }
    producer.join ();

    EXPECT_EQ (seen, kCount);
    EXPECT_EQ (q.PendingCount (), 0u);
    EXPECT_EQ (q.PendingBytes (), 0u);
}
