// Preview/GhPreviewIngest.cpp — what the host DOES with a preview message.
//
// The codec decides whether bytes are well formed and the cache decides what a
// well-formed message means. This is the third thing, and it is the one nothing
// else can check: the ORDER rules, the capability gate, the shared-memory
// segment's lifetime, and what gets written back down the pipe.
//
// Every failure asserted here is invisible in Archicad:
//
//   * a segment that stays mapped after a batch is a handle and an address-space
//     reservation leaked once per solve, and a slider drag is hundreds of solves;
//   * an ack that is not sent leaks the WORKER's segment instead, because the
//     worker holds it until one arrives;
//   * an ack sent early releases memory the worker is still writing into, and
//     the corruption surfaces as a wrong-looking mesh several batches later;
//   * a refusal that fires per message instead of per batch buries the first
//     reason under 200000 identical ones, and the first reason is the only one
//     that says what went wrong;
//   * a segment opened by a name the worker chose freely maps somebody else's
//     section object into Archicad and reads it as geometry.
//
// None of the five produces an error, a log line anyone would connect to the
// cause, or anything on screen a user could report.

#include "Preview/GhPreviewIngest.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace evp::preview;
using namespace evp::grasshopper::protocol;

namespace {

// The segment as a buffer, so the state machine can be driven without a kernel
// object. It counts opens and closes because the LIFETIME is the thing under
// test: GhPreviewSegmentView's own Open/Close is four Win32 calls, and the
// interesting question is whether it is called at the right moments.
class FakeSegment final : public GhPreviewSegmentSource {
  public:
    std::vector<uint8_t> content;
    bool failOpen = false;
    std::string lastName;
    uint32_t lastDeclared = 0;
    int opens = 0;
    int closes = 0;
    bool open = false;

    bool Open (const std::string& name, uint32_t declaredBytes, std::string& error) override
    {
        lastName = name;
        lastDeclared = declaredBytes;
        if (failOpen) {
            error = "The preview batch's shared memory \"" + name + "\" could not be opened.";
            return false;
        }
        if (content.size () < (std::size_t) declaredBytes) {
            error = "short segment";
            return false;
        }
        ++opens;
        open = true;
        return true;
    }

    const uint8_t* Data () const override
    {
        return open && !content.empty () ? content.data () : nullptr;
    }

    std::size_t Size () const override
    {
        return open ? (std::size_t) lastDeclared : 0;
    }

    void Close () override
    {
        if (open)
            ++closes;
        open = false;
    }
};

// A Polyline3D whose arrays are small enough to travel inline.
PreviewPrimitiveMessage InlineLine (uint64_t id, uint32_t revision, float shift)
{
    PreviewPrimitiveMessage message;
    message.header.primitiveId = id;
    message.header.kind = PreviewKind::Polyline3D;
    message.header.flags = PreviewFlagVisible | PreviewFlagDepthTest;
    message.header.surface = PreviewSurface::Model3D;
    message.header.revision = revision;
    message.header.contentHash = 7000 + id;
    message.descriptor.positionFloats = 6;
    message.positions = { shift, 0.0f, 0.0f, shift + 1.0f, 0.0f, 0.0f };
    return message;
}

// A TriangleMesh whose arrays live in the batch segment — the volume case, and
// the whole reason shared memory exists on this path.
PreviewPrimitiveMessage SegmentMesh (uint64_t id, uint32_t revision, std::vector<uint8_t>& segment)
{
    const std::vector<float> positions = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    const std::vector<uint32_t> indices = { 0, 1, 2 };

    PreviewPrimitiveMessage message;
    message.header.primitiveId = id;
    message.header.kind = PreviewKind::TriangleMesh;
    message.header.flags = PreviewFlagVisible | PreviewFlagDepthTest;
    message.header.surface = PreviewSurface::Model3D;
    message.header.revision = revision;
    message.header.contentHash = 9000 + id;
    message.descriptor.positionFloats = (uint32_t) positions.size ();
    message.descriptor.indexCount = (uint32_t) indices.size ();
    message.inSegment = true;
    message.header.segmentOffset = (uint64_t) segment.size ();
    message.header.segmentBytes = (uint32_t) (positions.size () * 4 + indices.size () * 4);

    for (const float value : positions) {
        uint8_t bytes[4];
        std::memcpy (bytes, &value, 4);
        segment.insert (segment.end (), bytes, bytes + 4);
    }
    for (const uint32_t value : indices) {
        segment.push_back ((uint8_t) (value & 0xFF));
        segment.push_back ((uint8_t) ((value >> 8) & 0xFF));
        segment.push_back ((uint8_t) ((value >> 16) & 0xFF));
        segment.push_back ((uint8_t) ((value >> 24) & 0xFF));
    }
    return message;
}

std::vector<uint8_t> BeginBytes (uint32_t epoch, uint32_t revision, uint32_t count, uint32_t segmentBytes,
                                 const std::string& name)
{
    PreviewBeginBatchPayload begin;
    begin.epoch = epoch;
    begin.revision = revision;
    begin.primitiveCount = count;
    begin.segmentBytes = segmentBytes;
    begin.segmentName = name;
    return EncodePreviewBeginBatch (begin);
}

std::string SegmentName (uint32_t epoch, uint32_t revision)
{
    return std::string (PreviewSegmentNamePrefix) + std::to_string (epoch) + "." + std::to_string (revision);
}

// The footer as the WORKER computes it: over what was sent, in send order. A
// test that recomputed it from what the host accumulated would prove nothing.
struct Footer {
    uint64_t checksum = PreviewChecksumStart ();
    uint32_t entries = 0;

    void Add (uint64_t id, PreviewChange change)
    {
        checksum = PreviewChecksumAccumulate (checksum, id, change);
        ++entries;
    }

    std::vector<uint8_t> Bytes (uint32_t epoch, uint32_t revision) const
    {
        PreviewEndBatchPayload end;
        end.epoch = epoch;
        end.revision = revision;
        end.entryCount = entries;
        end.checksum = checksum;
        return EncodePreviewEndBatch (end);
    }
};

// One ingest, its cache and its segment, wired the way GhBridge wires them.
struct Fixture {
    GhPreviewCache cache;
    FakeSegment segment;
    GhPreviewIngest ingest { cache, segment };

    Fixture ()
    {
        ingest.GrantCapabilities (CapabilityPreview);
    }

    GhPreviewReply Send (MessageType type, const std::vector<uint8_t>& payload)
    {
        return ingest.OnMessage (type, payload.empty () ? nullptr : payload.data (), payload.size ());
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The capability gate
// ---------------------------------------------------------------------------

// "Off costs nothing" is a rule about the HANDSHAKE, so a preview message from a
// worker that was not granted the capability is a worker whose build disagrees
// with this one. Dropping it silently would hide exactly that.
TEST (GhPreviewIngest, RefusesEveryPreviewMessageWhenTheCapabilityWasNotGranted)
{
    Fixture fixture;
    fixture.ingest.GrantCapabilities (0);
    ASSERT_FALSE (fixture.ingest.PreviewGranted ());

    const GhPreviewReply reply = fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 0, ""));

    EXPECT_FALSE (reply.log.empty ());
    EXPECT_TRUE (reply.sendResync);
    EXPECT_FALSE (fixture.ingest.BatchOpen ());
    EXPECT_EQ (fixture.cache.Count (), 0u);
    EXPECT_EQ (fixture.segment.opens, 0);
}

// ---------------------------------------------------------------------------
// The happy path, inline and through the segment
// ---------------------------------------------------------------------------

TEST (GhPreviewIngest, AppliesAnInlineBatchAndAcknowledgesItOnce)
{
    Fixture fixture;
    Footer footer;

    EXPECT_FALSE (fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 0, "")).sendAck);
    EXPECT_TRUE (fixture.ingest.BatchOpen ());

    const PreviewPrimitiveMessage line = InlineLine (11, 1, 0.0f);
    EXPECT_FALSE (fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (line)).sendAck);
    footer.Add (11, PreviewChange::Added);

    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (1, 1));
    EXPECT_TRUE (end.sendAck);
    EXPECT_TRUE (end.ack.accepted);
    EXPECT_FALSE (end.sendResync);
    EXPECT_EQ (end.ack.epoch, 1u);
    EXPECT_EQ (end.ack.revision, 1u);

    EXPECT_FALSE (fixture.ingest.BatchOpen ());
    EXPECT_EQ (fixture.cache.Count (), 1u);
    // No segment was declared, so none was opened. A batch of small primitives
    // must not cost a shared-memory mapping.
    EXPECT_EQ (fixture.segment.opens, 0);
}

// The volume case: the mesh's arrays never travel on the pipe, and what reaches
// the cache is this process's own memory rather than a pointer into the
// worker's.
TEST (GhPreviewIngest, CopiesMeshArraysOutOfTheSegmentAndClosesItAtEndOfBatch)
{
    Fixture fixture;
    Footer footer;

    std::vector<uint8_t> segment;
    const PreviewPrimitiveMessage mesh = SegmentMesh (21, 4, segment);
    fixture.segment.content = segment;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (2, 4, 1, (uint32_t) segment.size (), SegmentName (2, 4)));
    EXPECT_EQ (fixture.segment.opens, 1);
    EXPECT_EQ (fixture.segment.lastName, SegmentName (2, 4));
    EXPECT_EQ (fixture.segment.lastDeclared, (uint32_t) segment.size ());

    EXPECT_TRUE (fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (mesh)).log.empty ());
    footer.Add (21, PreviewChange::Added);

    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (2, 4));
    EXPECT_TRUE (end.ack.accepted);

    // ⚠️ THE VIEW IS GONE BY THE TIME THE ACK IS SENT. The ack is what lets the
    // worker free the segment; a host still holding a view over it would be
    // reading memory the worker is entitled to reuse.
    EXPECT_EQ (fixture.segment.closes, 1);
    EXPECT_FALSE (fixture.segment.open);

    auto snapshot = fixture.cache.SnapshotCopy ();
    ASSERT_TRUE (snapshot != nullptr);
    ASSERT_EQ (snapshot->primitives.size (), 1u);
    EXPECT_EQ (snapshot->primitives[0]->kind, PreviewKind::TriangleMesh);
    EXPECT_EQ (snapshot->primitives[0]->positions.size (), 9u);
    EXPECT_EQ (snapshot->primitives[0]->indices.size (), 3u);
    EXPECT_FLOAT_EQ (snapshot->primitives[0]->positions[3], 1.0f);
}

// ⚠️ THE ONLY POSITIVE EVIDENCE THE WHOLE PATH PRODUCES. Every other log line
// here is a failure, so without this one "nothing in grasshopper.log" means both
// "it is working" and "nothing ever arrived" -- and the first live report of
// this feature was exactly that ambiguity. Once per generation, not once per
// batch: a slider drag is hundreds of batches and would bury everything else.
TEST (GhPreviewIngest, TheFirstAcceptedBatchOfAGenerationSaysSoAndTheRestAreQuiet)
{
    Fixture fixture;

    Footer first;
    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (201, 1, 0.0f)));
    first.Add (201, PreviewChange::Added);
    const GhPreviewReply accepted = fixture.Send (MessageType::PreviewEndBatch, first.Bytes (1, 1));
    ASSERT_TRUE (accepted.ack.accepted);
    EXPECT_NE (accepted.log.find ("preview accepted"), std::string::npos);

    Footer second;
    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 2, 1, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (202, 2, 5.0f)));
    second.Add (202, PreviewChange::Added);
    const GhPreviewReply quiet = fixture.Send (MessageType::PreviewEndBatch, second.Bytes (1, 2));
    ASSERT_TRUE (quiet.ack.accepted);
    EXPECT_TRUE (quiet.log.empty ());

    // A fresh worker says it again -- that generation has its own evidence to
    // give, and it is the one the user is now looking at.
    fixture.ingest.OnWorkerGone ("the worker was restarted");
    Footer restarted;
    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (9, 1, 1, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (203, 1, 0.0f)));
    restarted.Add (203, PreviewChange::Added);
    const GhPreviewReply again = fixture.Send (MessageType::PreviewEndBatch, restarted.Bytes (9, 1));
    ASSERT_TRUE (again.ack.accepted);
    EXPECT_NE (again.log.find ("preview accepted"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Segment naming: the trust boundary
// ---------------------------------------------------------------------------

TEST (GhPreviewIngest, RefusesASegmentNameThatCouldAddressAnotherObjectNamespace)
{
    std::string error;

    EXPECT_TRUE (AcceptablePreviewSegmentName (SegmentName (3, 9), error));

    // The whole reason the rule exists: a namespace prefix would let a batch
    // point OpenFileMapping at somebody else's section object.
    EXPECT_FALSE (AcceptablePreviewSegmentName ("Global\\Tapioca.GhPreview.1.1", error));
    EXPECT_FALSE (AcceptablePreviewSegmentName ("Tapioca.GhPreview.1\\..\\other", error));
    // Not this add-on's prefix at all.
    EXPECT_FALSE (AcceptablePreviewSegmentName ("SomeOtherApp.Buffer", error));
    // The prefix and nothing else identifies no particular batch.
    EXPECT_FALSE (AcceptablePreviewSegmentName (PreviewSegmentNamePrefix, error));
    EXPECT_FALSE (error.empty ());
}

TEST (GhPreviewIngest, ABatchNamingAnUnacceptableSegmentIsSpoiledBeforeAnythingIsOpened)
{
    Fixture fixture;

    const GhPreviewReply begin =
        fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 64, "Global\\Tapioca.GhPreview.1.1"));
    EXPECT_FALSE (begin.log.empty ());
    EXPECT_EQ (fixture.segment.opens, 0);

    // The batch is still consumed to its end, and the ack that releases the
    // worker's segment is still sent — refusing a batch must not strand the
    // worker holding memory.
    Footer footer;
    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (1, 1));
    EXPECT_TRUE (end.sendAck);
    EXPECT_FALSE (end.ack.accepted);
    EXPECT_TRUE (end.sendResync);
    EXPECT_EQ (fixture.cache.Count (), 0u);
}

TEST (GhPreviewIngest, ASegmentThatCannotBeOpenedSpoilsTheBatchRatherThanDrawingHalfOfIt)
{
    Fixture fixture;
    fixture.segment.failOpen = true;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 128, SegmentName (1, 1)));

    std::vector<uint8_t> segment;
    const PreviewPrimitiveMessage mesh = SegmentMesh (31, 1, segment);
    // Skipped without a second reason: the batch is already spoiled.
    EXPECT_TRUE (fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (mesh)).log.empty ());

    Footer footer;
    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (1, 1));
    EXPECT_TRUE (end.sendAck);
    EXPECT_FALSE (end.ack.accepted);
    EXPECT_NE (end.ack.reason.find ("could not be opened"), std::string::npos);
    EXPECT_EQ (fixture.cache.Count (), 0u);
}

// ---------------------------------------------------------------------------
// Order and atomicity
// ---------------------------------------------------------------------------

TEST (GhPreviewIngest, APrimitiveOutsideABatchIsRefusedAndNothingIsStaged)
{
    Fixture fixture;

    const PreviewPrimitiveMessage line = InlineLine (41, 1, 0.0f);
    const GhPreviewReply reply = fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (line));

    EXPECT_FALSE (reply.log.empty ());
    EXPECT_FALSE (reply.sendAck);
    EXPECT_TRUE (reply.sendResync);
    EXPECT_EQ (fixture.cache.Count (), 0u);
}

// The worker holds the segment for a batch until an ack names it, so a footer
// with no opening still has to be answered — otherwise that memory is stranded
// for as long as the worker lives.
TEST (GhPreviewIngest, AFooterWithNoOpeningIsStillAcknowledgedSoTheWorkerCanReleaseItsSegment)
{
    Fixture fixture;
    Footer footer;

    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (5, 7));

    EXPECT_TRUE (end.sendAck);
    EXPECT_FALSE (end.ack.accepted);
    EXPECT_EQ (end.ack.epoch, 5u);
    EXPECT_EQ (end.ack.revision, 7u);
    EXPECT_TRUE (end.sendResync);
}

TEST (GhPreviewIngest, ASecondBeginSpoilsTheBatchInsteadOfNestingOne)
{
    Fixture fixture;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 0, ""));
    const GhPreviewReply second = fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 2, 1, 0, ""));

    EXPECT_FALSE (second.log.empty ());
    EXPECT_FALSE (second.sendAck);
    EXPECT_EQ (fixture.cache.Count (), 0u);
}

// One reason, not one per primitive. The first reason is the only one that says
// what actually went wrong, and burying it under thousands of copies is how a
// diagnosable failure becomes an unreadable log.
TEST (GhPreviewIngest, ASpoiledBatchProducesExactlyOneReasonHoweverManyMessagesFollow)
{
    Fixture fixture;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 4, 0, ""));

    // A primitive claiming a revision that is not the batch's: the cache
    // refuses it, and that spoils the batch.
    const PreviewPrimitiveMessage wrong = InlineLine (51, 99, 0.0f);
    const GhPreviewReply first = fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (wrong));
    EXPECT_FALSE (first.log.empty ());

    int extraReasons = 0;
    for (uint64_t id = 52; id < 56; ++id) {
        const GhPreviewReply reply =
            fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (id, 1, 1.0f)));
        if (!reply.log.empty ())
            ++extraReasons;
    }
    EXPECT_EQ (extraReasons, 0);

    Footer footer;
    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (1, 1));
    EXPECT_EQ (end.ack.reason, first.log);
}

// The viewport keeps showing the last COMPLETE preview, which is the last
// picture that was ever true.
TEST (GhPreviewIngest, ASpoiledBatchLeavesTheLastGoodPreviewStanding)
{
    Fixture fixture;
    Footer good;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (61, 1, 0.0f)));
    good.Add (61, PreviewChange::Added);
    ASSERT_TRUE (fixture.Send (MessageType::PreviewEndBatch, good.Bytes (1, 1)).ack.accepted);
    ASSERT_EQ (fixture.cache.Count (), 1u);

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 2, 2, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (62, 2, 5.0f)));
    // A footer whose checksum does not match what the host accumulated.
    Footer wrong;
    wrong.Add (999, PreviewChange::Added);
    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, wrong.Bytes (1, 2));

    EXPECT_FALSE (end.ack.accepted);
    EXPECT_TRUE (end.sendResync);
    EXPECT_EQ (fixture.cache.Count (), 1u);
    auto snapshot = fixture.cache.SnapshotCopy ();
    ASSERT_EQ (snapshot->primitives.size (), 1u);
    EXPECT_EQ (snapshot->primitives[0]->id, 61u);
}

// ---------------------------------------------------------------------------
// Epochs, drops and worker death
// ---------------------------------------------------------------------------

// A stale epoch spoils the batch WITHOUT asking for a resync: it is what a
// restart looks like from here, and asking a worker that has already moved on to
// resend a dead generation is worse than useless.
TEST (GhPreviewIngest, AStaleEpochIsAcknowledgedWithoutAskingForAResync)
{
    Fixture fixture;
    Footer footer;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (7, 1, 1, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (71, 1, 0.0f)));
    footer.Add (71, PreviewChange::Added);
    ASSERT_TRUE (fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (7, 1)).ack.accepted);

    const GhPreviewReply begin = fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (6, 1, 1, 0, ""));
    EXPECT_FALSE (begin.log.empty ());
    EXPECT_FALSE (begin.sendResync);

    Footer stale;
    const GhPreviewReply end = fixture.Send (MessageType::PreviewEndBatch, stale.Bytes (6, 1));
    EXPECT_TRUE (end.sendAck);
    EXPECT_FALSE (end.ack.accepted);
    EXPECT_FALSE (end.sendResync);
    // The epoch-7 preview is untouched.
    EXPECT_EQ (fixture.cache.Count (), 1u);
}

TEST (GhPreviewIngest, DropAllForgetsEverythingClosesTheSegmentAndIsNeverAcknowledged)
{
    Fixture fixture;
    Footer footer;

    std::vector<uint8_t> segment;
    const PreviewPrimitiveMessage mesh = SegmentMesh (81, 1, segment);
    fixture.segment.content = segment;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, (uint32_t) segment.size (), SegmentName (1, 1)));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (mesh));
    footer.Add (81, PreviewChange::Added);
    ASSERT_TRUE (fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (1, 1)).ack.accepted);
    ASSERT_EQ (fixture.cache.Count (), 1u);

    // A second batch, left in flight, with its segment mapped.
    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 2, 1, (uint32_t) segment.size (), SegmentName (1, 2)));
    ASSERT_TRUE (fixture.segment.open);

    PreviewDropAllPayload drop;
    drop.epoch = 2;
    drop.reason = "the definition was closed";
    const GhPreviewReply reply = fixture.Send (MessageType::PreviewDropAll, EncodePreviewDropAll (drop));

    EXPECT_FALSE (reply.sendAck);
    EXPECT_FALSE (reply.sendResync);
    EXPECT_EQ (reply.log, "the definition was closed");
    EXPECT_FALSE (fixture.segment.open);
    EXPECT_EQ (fixture.segment.closes, 2);
    EXPECT_FALSE (fixture.ingest.BatchOpen ());
    EXPECT_EQ (fixture.cache.Count (), 0u);
    EXPECT_EQ (fixture.cache.Epoch (), 2u);
}

// The worker died mid-batch. Its segment must not stay mapped: it is a handle
// and an address-space reservation belonging to a process that no longer exists.
TEST (GhPreviewIngest, WorkerDeathUnmapsTheSegmentAndForgetsThePreview)
{
    Fixture fixture;
    Footer footer;

    std::vector<uint8_t> segment;
    const PreviewPrimitiveMessage mesh = SegmentMesh (91, 1, segment);
    fixture.segment.content = segment;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, (uint32_t) segment.size (), SegmentName (1, 1)));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (mesh));
    footer.Add (91, PreviewChange::Added);
    ASSERT_TRUE (fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (1, 1)).ack.accepted);

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 2, 1, (uint32_t) segment.size (), SegmentName (1, 2)));
    ASSERT_TRUE (fixture.segment.open);

    fixture.ingest.OnWorkerGone ("the worker stopped answering");

    EXPECT_FALSE (fixture.segment.open);
    EXPECT_FALSE (fixture.ingest.BatchOpen ());
    EXPECT_EQ (fixture.cache.Count (), 0u);

    // And the host is ready for a fresh generation rather than stuck refusing
    // it: epoch 0 adopts whatever the next worker says.
    Footer restarted;
    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (12, 1, 1, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (92, 1, 0.0f)));
    restarted.Add (92, PreviewChange::Added);
    EXPECT_TRUE (fixture.Send (MessageType::PreviewEndBatch, restarted.Bytes (12, 1)).ack.accepted);
    EXPECT_EQ (fixture.cache.Count (), 1u);
}

// ---------------------------------------------------------------------------
// Flags: the cheap messages
// ---------------------------------------------------------------------------

// Clicking a component on the canvas does not re-solve, so a selection has no
// batch to ride in. Refusing it for arriving between batches would make canvas
// selection silently do nothing.
TEST (GhPreviewIngest, SelectionAppliesBetweenBatchesAndMovesNoGeometry)
{
    Fixture fixture;
    Footer footer;

    fixture.Send (MessageType::PreviewBeginBatch, BeginBytes (1, 1, 1, 0, ""));
    fixture.Send (MessageType::PreviewAdded, EncodePreviewPrimitive (InlineLine (101, 1, 0.0f)));
    footer.Add (101, PreviewChange::Added);
    ASSERT_TRUE (fixture.Send (MessageType::PreviewEndBatch, footer.Bytes (1, 1)).ack.accepted);

    auto before = fixture.cache.SnapshotCopy ();
    ASSERT_EQ (before->primitives.size (), 1u);
    const std::vector<float> geometry = before->primitives[0]->positions;

    PreviewIdRunPayload run;
    run.epoch = 1;
    run.revision = 1;
    run.flagValue = PreviewFlagSelected;
    run.flagMask = PreviewFlagSelected;
    run.ids = { 101 };
    const GhPreviewReply reply = fixture.Send (MessageType::PreviewSelection, EncodePreviewIdRun (run, true));

    EXPECT_TRUE (reply.log.empty ());
    EXPECT_FALSE (reply.sendResync);
    EXPECT_FALSE (fixture.ingest.BatchOpen ());

    auto after = fixture.cache.SnapshotCopy ();
    ASSERT_EQ (after->primitives.size (), 1u);
    EXPECT_TRUE (after->primitives[0]->Selected ());
    EXPECT_EQ (after->primitives[0]->positions, geometry);
}
