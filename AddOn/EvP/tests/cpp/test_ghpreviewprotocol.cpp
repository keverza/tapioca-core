// Grasshopper/GhPreviewProtocol.cpp — the preview half of the Archicad <->
// Tapioca.GhWorker.exe wire.
//
// The usual framing argument (test_ghprotocol.cpp) applies here with one edge
// added, and it is the sharp one: on THIS path the worker is the producer.
// Preview flows worker -> host, the worker is a separate process running
// third-party components, and its declared counts, offsets and lengths end up
// indexing into memory inside Archicad.exe. A length reconciled against the
// wrong number here is not a message read as another message — it is an
// out-of-bounds read in the host process.
//
// So the round trips below are the cheap half. The REFUSALS are the point, and
// the segment-bounds cases are the point of the point.

#include "Grasshopper/GhPreviewProtocol.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace evp::grasshopper::protocol;

namespace {

PreviewPrimitiveMessage Polyline (uint64_t id, uint32_t points)
{
    PreviewPrimitiveMessage message;
    message.header.primitiveId = id;
    message.header.kind = PreviewKind::Polyline3D;
    message.header.flags = PreviewFlagVisible | PreviewFlagDepthTest;
    message.header.revision = 1;
    message.descriptor.positionFloats = points * 3;
    message.descriptor.closed = false;
    for (uint32_t index = 0; index < points * 3; ++index)
        message.positions.push_back ((float) index);
    return message;
}

// One triangle, arrays destined for the batch segment.
PreviewPrimitiveMessage MeshInSegment (uint64_t id, uint64_t offset)
{
    PreviewPrimitiveMessage message;
    message.header.primitiveId = id;
    message.header.kind = PreviewKind::TriangleMesh;
    message.header.revision = 1;
    message.header.segmentOffset = offset;
    message.descriptor.positionFloats = 9;
    message.descriptor.normalFloats = 9;
    message.descriptor.indexCount = 3;
    message.inSegment = true;
    return message;
}

std::vector<uint8_t> SegmentFor (const PreviewPrimitiveMessage& message,
                                 const std::vector<float>& positions,
                                 const std::vector<float>& normals,
                                 const std::vector<uint32_t>& indices,
                                 size_t totalSize)
{
    std::vector<uint8_t> segment (totalSize, 0);
    size_t cursor = (size_t) message.header.segmentOffset;
    for (float value : positions) {
        std::memcpy (segment.data () + cursor, &value, 4);
        cursor += 4;
    }
    for (float value : normals) {
        std::memcpy (segment.data () + cursor, &value, 4);
        cursor += 4;
    }
    for (uint32_t value : indices) {
        std::memcpy (segment.data () + cursor, &value, 4);
        cursor += 4;
    }
    return segment;
}

} // namespace

TEST (GhPreviewProtocol, APrimitiveRoundTripsEveryHeaderField)
{
    PreviewPrimitiveMessage sent = Polyline (0x1122334455667788ull, 4);
    sent.header.itemIndex = 17;
    sent.header.branchHash = 0xABCDEF01u;
    sent.header.contentHash = 0x0102030405060708ull;
    sent.header.revision = 9;
    sent.descriptor.closed = true;
    for (int index = 0; index < 16; ++index) {
        sent.header.componentGuid[index] = (uint8_t) index;
        sent.header.parameterGuid[index] = (uint8_t) (index + 100);
    }

    const std::vector<uint8_t> bytes = EncodePreviewPrimitive (sent);
    PreviewPrimitiveMessage got;
    std::string error;
    ASSERT_TRUE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error)) << error;

    EXPECT_EQ (sent.header.primitiveId, got.header.primitiveId);
    EXPECT_EQ (PreviewKind::Polyline3D, got.header.kind);
    EXPECT_EQ (sent.header.flags, got.header.flags);
    EXPECT_EQ (17u, got.header.itemIndex);
    EXPECT_EQ (0xABCDEF01u, got.header.branchHash);
    EXPECT_EQ (0x0102030405060708ull, got.header.contentHash);
    EXPECT_EQ (9u, got.header.revision);
    EXPECT_TRUE (got.descriptor.closed);
    EXPECT_EQ (0, std::memcmp (sent.header.componentGuid, got.header.componentGuid, 16));
    EXPECT_EQ (0, std::memcmp (sent.header.parameterGuid, got.header.parameterGuid, 16));
    EXPECT_EQ (sent.positions, got.positions);
    EXPECT_FALSE (got.inSegment);
}

TEST (GhPreviewProtocol, ThePrimitiveHeaderIsEightyLittleEndianBytes)
{
    // Spelled out rather than round-tripped, for the reason test_ghprotocol.cpp
    // gives: the two halves are a C++ .apx and a C# .gha with no shared header,
    // so "both ends agree" is only true if the bytes are pinned. A change here
    // is a change to PreviewChannel.cs in the same edit.
    PreviewPrimitiveMessage message = Polyline (0x0807060504030201ull, 2);
    const std::vector<uint8_t> bytes = EncodePreviewPrimitive (message);

    ASSERT_GE (bytes.size (), (size_t) PreviewHeaderSize);
    EXPECT_EQ (0x01u, bytes[0]);
    EXPECT_EQ (0x08u, bytes[7]);
    EXPECT_EQ ((uint8_t) PreviewKind::Polyline3D, bytes[8]);
    // The descriptor starts exactly at byte 80, whatever the kind.
    EXPECT_EQ (6u, (uint32_t) bytes[PreviewHeaderSize]); // positionFloats == 2 points * 3
}

TEST (GhPreviewProtocol, AnUnknownPrimitiveKindIsRefusedBeforeAnythingIsReadThroughIt)
{
    std::vector<uint8_t> bytes = EncodePreviewPrimitive (Polyline (1, 2));
    bytes[8] = 99;

    PreviewPrimitiveMessage got;
    std::string error;
    EXPECT_FALSE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error));
    EXPECT_NE (std::string::npos, error.find ("99"));
}

TEST (GhPreviewProtocol, APayloadThatDescribesMoreThanItCarriesIsRefused)
{
    // The classic over-read: declared lengths reconciled against each other
    // rather than against what actually arrived.
    std::vector<uint8_t> bytes = EncodePreviewPrimitive (Polyline (1, 4));
    bytes.pop_back ();

    PreviewPrimitiveMessage got;
    std::string error;
    EXPECT_FALSE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error));
    EXPECT_FALSE (error.empty ());
}

TEST (GhPreviewProtocol, AnOversizedPayloadIsRefusedBeforeAllocation)
{
    std::vector<uint8_t> bytes = EncodePreviewPrimitive (Polyline (1, 2));
    const uint32_t huge = MaxPreviewInlinePayloadBytes + 1u;
    bytes[64] = (uint8_t) (huge & 0xFFu);
    bytes[65] = (uint8_t) ((huge >> 8) & 0xFFu);
    bytes[66] = (uint8_t) ((huge >> 16) & 0xFFu);
    bytes[67] = (uint8_t) ((huge >> 24) & 0xFFu);

    PreviewPrimitiveMessage got;
    std::string error;
    EXPECT_FALSE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error));
    EXPECT_NE (std::string::npos, error.find ("limit"));
}

TEST (GhPreviewProtocol, EachKindMayOnlyCarryWhatItMeans)
{
    // The point of the rule table: the host builds a plane's axes from exactly
    // three points, and would build them from whatever the first three happened
    // to be if a mesh could arrive claiming to be one.
    std::string error;
    PreviewPayloadDescriptor plane;
    plane.positionFloats = 12;
    EXPECT_FALSE (ValidatePreviewPayload (PreviewKind::PlaneGizmo, plane, error));

    PreviewPayloadDescriptor marker;
    marker.positionFloats = 6;
    EXPECT_FALSE (ValidatePreviewPayload (PreviewKind::PointMarker, marker, error));

    PreviewPayloadDescriptor line;
    line.positionFloats = 3;
    EXPECT_FALSE (ValidatePreviewPayload (PreviewKind::Polyline3D, line, error));

    PreviewPayloadDescriptor mesh;
    mesh.positionFloats = 9;
    mesh.indexCount = 4; // not a whole number of triangles
    EXPECT_FALSE (ValidatePreviewPayload (PreviewKind::TriangleMesh, mesh, error));

    PreviewPayloadDescriptor indexedLine;
    indexedLine.positionFloats = 6;
    indexedLine.indexCount = 3;
    EXPECT_FALSE (ValidatePreviewPayload (PreviewKind::Polyline3D, indexedLine, error));

    PreviewPayloadDescriptor emptyText;
    emptyText.positionFloats = 3;
    EXPECT_FALSE (ValidatePreviewPayload (PreviewKind::BillboardText, emptyText, error));

    PreviewPayloadDescriptor good;
    good.positionFloats = 9;
    good.normalFloats = 9;
    good.indexCount = 3;
    EXPECT_TRUE (ValidatePreviewPayload (PreviewKind::TriangleMesh, good, error)) << error;
}

TEST (GhPreviewProtocol, NormalsMustMatchTheirPositions)
{
    std::string error;
    PreviewPayloadDescriptor descriptor;
    descriptor.positionFloats = 9;
    descriptor.normalFloats = 6;
    descriptor.indexCount = 3;
    EXPECT_FALSE (ValidatePreviewPayload (PreviewKind::TriangleMesh, descriptor, error));
}

TEST (GhPreviewProtocol, AnIndexPastTheVertexCountIsRefused)
{
    // The one remaining way a malformed worker reaches memory it does not own,
    // and it survives every length check above because the LENGTHS are right.
    PreviewPrimitiveMessage message;
    message.header.kind = PreviewKind::TriangleMesh;
    message.descriptor.positionFloats = 9;
    message.descriptor.indexCount = 3;
    message.positions.assign (9, 0.0f);
    message.indices = { 0, 1, 3 };

    std::string error;
    EXPECT_FALSE (ValidatePreviewIndices (message, error));
    EXPECT_NE (std::string::npos, error.find ("indexed vertex 3"));
}

TEST (GhPreviewProtocol, ASegmentResidentPrimitiveArrivesWithoutItsArrays)
{
    PreviewPrimitiveMessage sent = MeshInSegment (5, 128);
    const std::vector<uint8_t> bytes = EncodePreviewPrimitive (sent);
    // Descriptor only on the pipe: the bulk is what shared memory is for.
    EXPECT_EQ ((size_t) (PreviewHeaderSize + PreviewDescriptorSize), bytes.size ());

    PreviewPrimitiveMessage got;
    std::string error;
    ASSERT_TRUE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error)) << error;
    EXPECT_TRUE (got.inSegment);
    EXPECT_TRUE (got.positions.empty ());
    EXPECT_EQ (128u, got.header.segmentOffset);
    EXPECT_EQ (9u * 4u + 9u * 4u + 3u * 4u, got.header.segmentBytes);
}

TEST (GhPreviewProtocol, ASegmentPrimitiveIsCopiedOutAndValidated)
{
    PreviewPrimitiveMessage sent = MeshInSegment (5, 16);
    const std::vector<uint8_t> bytes = EncodePreviewPrimitive (sent);
    PreviewPrimitiveMessage got;
    std::string error;
    ASSERT_TRUE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error)) << error;

    const std::vector<float> positions = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    const std::vector<float> normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    const std::vector<uint32_t> indices = { 0, 1, 2 };
    const std::vector<uint8_t> segment = SegmentFor (got, positions, normals, indices, 256);

    ASSERT_TRUE (ReadPreviewSegment (segment.data (), segment.size (), got, error)) << error;
    EXPECT_EQ (positions, got.positions);
    EXPECT_EQ (normals, got.normals);
    EXPECT_EQ (indices, got.indices);
    // ⚠️ COPIED OUT, so the view can be unmapped and the batch acknowledged. The
    // host never hands a pointer into the worker's memory to the render thread.
    EXPECT_FALSE (got.inSegment);
}

TEST (GhPreviewProtocol, ASegmentRangePastTheEndIsRefused)
{
    PreviewPrimitiveMessage sent = MeshInSegment (5, 200);
    const std::vector<uint8_t> bytes = EncodePreviewPrimitive (sent);
    PreviewPrimitiveMessage got;
    std::string error;
    ASSERT_TRUE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error)) << error;

    const std::vector<uint8_t> segment (220, 0);
    EXPECT_FALSE (ReadPreviewSegment (segment.data (), segment.size (), got, error));
    EXPECT_NE (std::string::npos, error.find ("outside"));
}

TEST (GhPreviewProtocol, ASegmentOffsetPastTheEndCannotWrapItsWayBackIn)
{
    // offset + length in 32-bit arithmetic would wrap and land inside the
    // segment. The check is done in 64 bits for exactly this input.
    PreviewPrimitiveMessage got = MeshInSegment (5, 0);
    got.header.segmentOffset = 0xFFFFFFF0ull;
    got.header.segmentBytes = 48;

    std::string error;
    const std::vector<uint8_t> segment (256, 0);
    EXPECT_FALSE (ReadPreviewSegment (segment.data (), segment.size (), got, error));
}

TEST (GhPreviewProtocol, AnUnmappedSegmentIsARefusalRatherThanACrash)
{
    PreviewPrimitiveMessage got = MeshInSegment (5, 0);
    std::string error;
    EXPECT_FALSE (ReadPreviewSegment (nullptr, 0, got, error));
    EXPECT_FALSE (error.empty ());
}

TEST (GhPreviewProtocol, APrimitiveCannotDeclareASegmentAndInlineArraysAtOnce)
{
    std::vector<uint8_t> bytes = EncodePreviewPrimitive (Polyline (1, 4));
    // Give the inline primitive a segment length as well.
    bytes[76] = 12;

    PreviewPrimitiveMessage got;
    std::string error;
    EXPECT_FALSE (DecodePreviewPrimitive (bytes.data (), bytes.size (), got, error));
}

TEST (GhPreviewProtocol, ABatchHeaderRoundTripsAndRefusesAFloodBeforeAllocating)
{
    PreviewBeginBatchPayload begin;
    begin.epoch = 3;
    begin.revision = 12;
    begin.primitiveCount = 40;
    begin.segmentBytes = 4096;
    begin.segmentName = "Tapioca.GhPreview.3.12";

    const std::vector<uint8_t> bytes = EncodePreviewBeginBatch (begin);
    PreviewBeginBatchPayload got;
    std::string error;
    ASSERT_TRUE (DecodePreviewBeginBatch (bytes.data (), bytes.size (), got, error)) << error;
    EXPECT_EQ (3u, got.epoch);
    EXPECT_EQ (12u, got.revision);
    EXPECT_EQ (40u, got.primitiveCount);
    EXPECT_EQ (4096u, got.segmentBytes);
    EXPECT_EQ (begin.segmentName, got.segmentName);

    // A definition previewing millions of primitives is the case the ceiling
    // exists for, and it must be refused rather than reserved.
    std::vector<uint8_t> flood = bytes;
    const uint32_t many = MaxPreviewPrimitivesPerBatch + 1u;
    flood[8] = (uint8_t) (many & 0xFFu);
    flood[9] = (uint8_t) ((many >> 8) & 0xFFu);
    flood[10] = (uint8_t) ((many >> 16) & 0xFFu);
    flood[11] = (uint8_t) ((many >> 24) & 0xFFu);
    EXPECT_FALSE (DecodePreviewBeginBatch (flood.data (), flood.size (), got, error));
    EXPECT_NE (std::string::npos, error.find ("limit"));
}

TEST (GhPreviewProtocol, ABatchThatDeclaresASegmentMustNameIt)
{
    // A segment nobody can open is worse than no segment: every primitive
    // referencing it would fail separately, one log line each.
    PreviewBeginBatchPayload begin;
    begin.epoch = 1;
    begin.revision = 1;
    begin.segmentBytes = 1024;
    begin.segmentName.clear ();

    const std::vector<uint8_t> bytes = EncodePreviewBeginBatch (begin);
    PreviewBeginBatchPayload got;
    std::string error;
    EXPECT_FALSE (DecodePreviewBeginBatch (bytes.data (), bytes.size (), got, error));
}

TEST (GhPreviewProtocol, AnIdRunRoundTripsAndReconcilesItsCountAgainstItsPayload)
{
    PreviewIdRunPayload run;
    run.epoch = 2;
    run.revision = 7;
    run.ids = { 10, 20, 30 };

    std::vector<uint8_t> bytes = EncodePreviewIdRun (run, false);
    PreviewIdRunPayload got;
    std::string error;
    ASSERT_TRUE (DecodePreviewIdRun (bytes.data (), bytes.size (), false, got, error)) << error;
    EXPECT_EQ (run.ids, got.ids);

    bytes.pop_back ();
    EXPECT_FALSE (DecodePreviewIdRun (bytes.data (), bytes.size (), false, got, error));
}

TEST (GhPreviewProtocol, AFlagRunSaysWhichFlagsItSpeaksFor)
{
    PreviewIdRunPayload run;
    run.epoch = 2;
    run.revision = 7;
    run.flagValue = 0;
    run.flagMask = PreviewFlagVisible;
    run.ids = { 42 };

    const std::vector<uint8_t> bytes = EncodePreviewIdRun (run, true);
    PreviewIdRunPayload got;
    std::string error;
    ASSERT_TRUE (DecodePreviewIdRun (bytes.data (), bytes.size (), true, got, error)) << error;
    EXPECT_EQ (PreviewFlagVisible, got.flagMask);
    EXPECT_EQ (0u, got.flagValue);
    // Ids and flags only. Visibility is a byte, never the geometry again.
    EXPECT_EQ ((size_t) 24, bytes.size ());
}

TEST (GhPreviewProtocol, AFlagRunThatSetsBitsItDoesNotSpeakForIsRefused)
{
    PreviewIdRunPayload run;
    run.flagValue = PreviewFlagSelected;
    run.flagMask = PreviewFlagVisible;
    run.ids = { 1 };

    const std::vector<uint8_t> bytes = EncodePreviewIdRun (run, true);
    PreviewIdRunPayload got;
    std::string error;
    EXPECT_FALSE (DecodePreviewIdRun (bytes.data (), bytes.size (), true, got, error));
}

TEST (GhPreviewProtocol, ARefusedBatchAckMustNameAReason)
{
    // A preview that silently stops working with no reason recorded is the
    // failure mode this refusal exists to prevent.
    PreviewBatchAckPayload ack;
    ack.epoch = 1;
    ack.revision = 4;
    ack.accepted = false;
    ack.reason.clear ();

    const std::vector<uint8_t> bytes = EncodePreviewBatchAck (ack);
    PreviewBatchAckPayload got;
    std::string error;
    EXPECT_FALSE (DecodePreviewBatchAck (bytes.data (), bytes.size (), got, error));

    ack.reason = "The batch exceeded the primitive ceiling.";
    const std::vector<uint8_t> named = EncodePreviewBatchAck (ack);
    ASSERT_TRUE (DecodePreviewBatchAck (named.data (), named.size (), got, error)) << error;
    EXPECT_FALSE (got.accepted);
    EXPECT_EQ (ack.reason, got.reason);
}

TEST (GhPreviewProtocol, TheBatchChecksumIsTheSameFnvBothHalvesCompute)
{
    // Pinned rather than round-tripped: the worker computes this in C#
    // (PreviewBatch.Checksum) and the host recomputes it here, and the whole
    // point is that two independent implementations agree. A change to either
    // must fail this.
    uint64_t hash = PreviewChecksumStart ();
    EXPECT_EQ (14695981039346656037ull, hash);
    hash = PreviewChecksumAccumulate (hash, 1, PreviewChange::Added);
    hash = PreviewChecksumAccumulate (hash, 2, PreviewChange::Removed);

    uint64_t again = PreviewChecksumStart ();
    again = PreviewChecksumAccumulate (again, 1, PreviewChange::Added);
    again = PreviewChecksumAccumulate (again, 2, PreviewChange::Removed);
    EXPECT_EQ (hash, again);

    // Order matters, which is what makes a reordered message detectable at all.
    uint64_t swapped = PreviewChecksumStart ();
    swapped = PreviewChecksumAccumulate (swapped, 2, PreviewChange::Removed);
    swapped = PreviewChecksumAccumulate (swapped, 1, PreviewChange::Added);
    EXPECT_NE (hash, swapped);
}

TEST (GhPreviewProtocol, APickCarriesAnIdAndNothingElse)
{
    PreviewPickedPayload picked;
    picked.primitiveId = 0x7766554433221100ull;
    const std::vector<uint8_t> bytes = EncodePreviewPicked (picked);
    EXPECT_EQ ((size_t) 8, bytes.size ());

    PreviewPickedPayload got;
    std::string error;
    ASSERT_TRUE (DecodePreviewPicked (bytes.data (), bytes.size (), got, error)) << error;
    EXPECT_EQ (picked.primitiveId, got.primitiveId);
}

TEST (GhPreviewProtocol, EveryKindHasAName)
{
    // DescribePreviewKind feeds every refusal message above; an unnamed kind
    // would surface as "unknown" in exactly the diagnostic someone is reading.
    const PreviewKind kinds[] = { PreviewKind::TriangleMesh,  PreviewKind::Polyline3D,      PreviewKind::PointMarker,
                                  PreviewKind::PlaneGizmo,    PreviewKind::Arrow3D,         PreviewKind::BillboardText,
                                  PreviewKind::WorldText,     PreviewKind::PointCloud,      PreviewKind::BillboardSprite,
                                  PreviewKind::Bounds };
    for (size_t index = 0; index < sizeof (kinds) / sizeof (kinds[0]); ++index) {
        EXPECT_STRNE ("unknown", DescribePreviewKind (kinds[index]));
        EXPECT_TRUE (KnownPreviewKind ((uint8_t) kinds[index]));
    }
    EXPECT_FALSE (KnownPreviewKind (0));
    EXPECT_FALSE (KnownPreviewKind (11));
}
