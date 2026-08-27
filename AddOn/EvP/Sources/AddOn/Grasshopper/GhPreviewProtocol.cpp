#include "GhPreviewProtocol.hpp"

#include <cstring>

namespace evp {
namespace grasshopper {
namespace protocol {

namespace {

// Spelled out byte by byte rather than memcpy'd from a struct, for the reason
// GhProtocol.cpp gives: a packed struct would make the wire format depend on
// the compiler that built each half, and the two halves are a C++ .apx and a
// C# assembly that do not share one.
void AppendUInt32 (std::vector<uint8_t>& buffer, uint32_t value)
{
    buffer.push_back ((uint8_t) (value & 0xFFu));
    buffer.push_back ((uint8_t) ((value >> 8) & 0xFFu));
    buffer.push_back ((uint8_t) ((value >> 16) & 0xFFu));
    buffer.push_back ((uint8_t) ((value >> 24) & 0xFFu));
}

void AppendUInt64 (std::vector<uint8_t>& buffer, uint64_t value)
{
    AppendUInt32 (buffer, (uint32_t) (value & 0xFFFFFFFFull));
    AppendUInt32 (buffer, (uint32_t) ((value >> 32) & 0xFFFFFFFFull));
}

void AppendUInt16 (std::vector<uint8_t>& buffer, uint16_t value)
{
    buffer.push_back ((uint8_t) (value & 0xFFu));
    buffer.push_back ((uint8_t) ((value >> 8) & 0xFFu));
}

uint32_t ReadUInt32 (const uint8_t* bytes)
{
    return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

uint64_t ReadUInt64 (const uint8_t* bytes)
{
    return (uint64_t) ReadUInt32 (bytes) | ((uint64_t) ReadUInt32 (bytes + 4) << 32);
}

uint16_t ReadUInt16 (const uint8_t* bytes)
{
    return (uint16_t) ((uint16_t) bytes[0] | (uint16_t) ((uint16_t) bytes[1] << 8));
}

// IEEE-754 binary32, carried as its bit pattern. Every platform this ships on
// is IEEE-754 little-endian, and going through the bits rather than through a
// reinterpret of the struct keeps the format independent of how a compiler
// chose to lay anything out.
void AppendFloat (std::vector<uint8_t>& buffer, float value)
{
    uint32_t bits = 0;
    std::memcpy (&bits, &value, sizeof (bits));
    AppendUInt32 (buffer, bits);
}

float ReadFloat (const uint8_t* bytes)
{
    const uint32_t bits = ReadUInt32 (bytes);
    float value = 0.0f;
    std::memcpy (&value, &bits, sizeof (value));
    return value;
}

void AppendText (std::vector<uint8_t>& buffer, const std::string& text)
{
    buffer.insert (buffer.end (), text.begin (), text.end ());
}

bool ContainsNul (const std::string& text)
{
    return text.find (static_cast<char> (0)) != std::string::npos;
}

// The array bytes a descriptor declares. Returned as uint64 so that the sum
// cannot wrap before it is compared against a 32-bit limit -- a wrapped size
// that passes a ceiling check is exactly the bug these ceilings exist to stop.
uint64_t ArrayBytes (const PreviewPayloadDescriptor& descriptor)
{
    return (uint64_t) descriptor.positionFloats * 4ull + (uint64_t) descriptor.normalFloats * 4ull +
           (uint64_t) descriptor.indexCount * 4ull + (uint64_t) descriptor.textBytes;
}

} // namespace

// FNV-1a 64. The same constants PreviewHash uses in PreviewPrimitives.cs,
// written out here rather than shared, for the same reason the framing is.
namespace {

constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;

uint64_t HashByte (uint64_t hash, uint8_t value)
{
    return (hash ^ (uint64_t) value) * FnvPrime;
}

uint64_t HashUInt32 (uint64_t hash, uint32_t value)
{
    hash = HashByte (hash, (uint8_t) (value & 0xFFu));
    hash = HashByte (hash, (uint8_t) ((value >> 8) & 0xFFu));
    hash = HashByte (hash, (uint8_t) ((value >> 16) & 0xFFu));
    return HashByte (hash, (uint8_t) ((value >> 24) & 0xFFu));
}

uint64_t HashUInt64 (uint64_t hash, uint64_t value)
{
    hash = HashUInt32 (hash, (uint32_t) (value & 0xFFFFFFFFull));
    return HashUInt32 (hash, (uint32_t) ((value >> 32) & 0xFFFFFFFFull));
}

} // namespace

uint64_t PreviewChecksumStart ()
{
    return FnvOffset;
}

uint64_t PreviewChecksumAccumulate (uint64_t hash, uint64_t primitiveId, PreviewChange change)
{
    hash = HashUInt64 (hash, primitiveId);
    return HashByte (hash, (uint8_t) change);
}

bool KnownPreviewKind (uint8_t value)
{
    switch ((PreviewKind) value) {
        case PreviewKind::TriangleMesh:
        case PreviewKind::Polyline3D:
        case PreviewKind::PointMarker:
        case PreviewKind::PlaneGizmo:
        case PreviewKind::Arrow3D:
        case PreviewKind::BillboardText:
        case PreviewKind::WorldText:
        case PreviewKind::PointCloud:
        case PreviewKind::BillboardSprite:
        case PreviewKind::Bounds:
            return true;
    }
    return false;
}

bool KnownPreviewSurface (uint8_t value)
{
    switch ((PreviewSurface) value) {
        case PreviewSurface::Model3D:
        case PreviewSurface::FloorPlan:
        case PreviewSurface::Both:
            return true;
    }
    return false;
}

const char* DescribePreviewSurface (PreviewSurface surface)
{
    switch (surface) {
        case PreviewSurface::Model3D:
            return "the 3D window";
        case PreviewSurface::FloorPlan:
            return "the floor plan";
        case PreviewSurface::Both:
            return "both windows";
    }
    return "an unknown surface";
}

const char* DescribePreviewKind (PreviewKind kind)
{
    switch (kind) {
        case PreviewKind::TriangleMesh:
            return "TriangleMesh";
        case PreviewKind::Polyline3D:
            return "Polyline3D";
        case PreviewKind::PointMarker:
            return "PointMarker";
        case PreviewKind::PlaneGizmo:
            return "PlaneGizmo";
        case PreviewKind::Arrow3D:
            return "Arrow3D";
        case PreviewKind::BillboardText:
            return "BillboardText";
        case PreviewKind::WorldText:
            return "WorldText";
        case PreviewKind::PointCloud:
            return "PointCloud";
        case PreviewKind::BillboardSprite:
            return "BillboardSprite";
        case PreviewKind::Bounds:
            return "Bounds";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Per-kind rules
// ---------------------------------------------------------------------------

bool ValidatePreviewPayload (PreviewKind kind, const PreviewPayloadDescriptor& descriptor, std::string& error)
{
    const std::string name = DescribePreviewKind (kind);

    if (descriptor.positionFloats % 3u != 0u) {
        error = "A " + name + " declared " + std::to_string (descriptor.positionFloats) +
                " position floats, which is not a whole number of points.";
        return false;
    }
    if (descriptor.normalFloats != 0u && descriptor.normalFloats != descriptor.positionFloats) {
        error = "A " + name + " declared " + std::to_string (descriptor.normalFloats) + " normal floats against " +
                std::to_string (descriptor.positionFloats) + " position floats.";
        return false;
    }
    if (descriptor.textBytes > MaxPreviewTextBytes) {
        error = "A " + name + " declared " + std::to_string (descriptor.textBytes) + " bytes of text, over the " +
                std::to_string (MaxPreviewTextBytes) + "-byte limit.";
        return false;
    }

    const uint32_t points = descriptor.PointCount ();

    // What each kind MEANS, expressed as what it may carry. The point of the
    // table is that a mesh cannot arrive claiming to be a plane: the host builds
    // a plane's axes from exactly three points, and would build them from
    // whatever the first three happened to be otherwise.
    switch (kind) {
        case PreviewKind::TriangleMesh:
            if (points < 3u) {
                error = "A TriangleMesh carried " + std::to_string (points) + " vertices.";
                return false;
            }
            if (descriptor.indexCount == 0u || descriptor.indexCount % 3u != 0u) {
                error = "A TriangleMesh declared " + std::to_string (descriptor.indexCount) +
                        " indices, which is not a whole number of triangles.";
                return false;
            }
            break;

        case PreviewKind::Polyline3D:
            if (points < 2u) {
                error = "A Polyline3D carried " + std::to_string (points) + " points.";
                return false;
            }
            break;

        case PreviewKind::PointCloud:
            if (points < 1u) {
                error = "A PointCloud carried no points.";
                return false;
            }
            break;

        case PreviewKind::PointMarker:
        case PreviewKind::BillboardSprite:
            if (points != 1u) {
                error = "A " + name + " carried " + std::to_string (points) + " points; it takes exactly one.";
                return false;
            }
            break;

        case PreviewKind::Arrow3D:
        case PreviewKind::Bounds:
            if (points != 2u) {
                error = "A " + name + " carried " + std::to_string (points) + " points; it takes exactly two.";
                return false;
            }
            break;

        case PreviewKind::PlaneGizmo:
            // Origin plus two axis vectors. The AXES THEMSELVES ARE NOT SENT --
            // the host draws them at constant pixel width from these three
            // points, which is the whole reason PlaneGizmo is not three
            // polylines.
            if (points != 3u) {
                error = "A PlaneGizmo carried " + std::to_string (points) + " points; it takes an origin and two axes.";
                return false;
            }
            break;

        case PreviewKind::BillboardText:
            if (points != 1u) {
                error = "A BillboardText carried " + std::to_string (points) + " anchors; it takes exactly one.";
                return false;
            }
            if (descriptor.textBytes == 0u) {
                error = "A BillboardText carried no text.";
                return false;
            }
            break;

        case PreviewKind::WorldText:
            if (points != 3u) {
                error = "A WorldText carried " + std::to_string (points) + " points; it takes a plane.";
                return false;
            }
            if (descriptor.textBytes == 0u) {
                error = "A WorldText carried no text.";
                return false;
            }
            break;
    }

    // Only a mesh indexes anything, and only a mesh and a cloud carry normals.
    // Both restrictions exist so that the host's per-kind draw path can trust
    // its inputs without re-deriving them.
    if (kind != PreviewKind::TriangleMesh && descriptor.indexCount != 0u) {
        error = "A " + name + " carried " + std::to_string (descriptor.indexCount) + " indices; only a mesh may.";
        return false;
    }
    if (kind != PreviewKind::TriangleMesh && kind != PreviewKind::PointCloud && descriptor.normalFloats != 0u) {
        error = "A " + name + " carried normals; only a mesh or a cloud may.";
        return false;
    }
    if (kind != PreviewKind::BillboardText && kind != PreviewKind::WorldText && descriptor.textBytes != 0u) {
        error = "A " + name + " carried text; only the text primitives may.";
        return false;
    }

    return true;
}

bool ValidatePreviewIndices (const PreviewPrimitiveMessage& message, std::string& error)
{
    const uint32_t points = message.descriptor.PointCount ();
    for (size_t index = 0; index < message.indices.size (); ++index) {
        if (message.indices[index] >= points) {
            error = "A " + std::string (DescribePreviewKind (message.header.kind)) + " indexed vertex " +
                    std::to_string (message.indices[index]) + " of " + std::to_string (points) + ".";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Batch control
// ---------------------------------------------------------------------------

std::vector<uint8_t> EncodePreviewBeginBatch (const PreviewBeginBatchPayload& begin)
{
    std::vector<uint8_t> payload;
    payload.reserve (20 + begin.segmentName.size ());
    AppendUInt32 (payload, begin.epoch);
    AppendUInt32 (payload, begin.revision);
    AppendUInt32 (payload, begin.primitiveCount);
    AppendUInt32 (payload, begin.segmentBytes);
    AppendUInt32 (payload, (uint32_t) begin.segmentName.size ());
    AppendText (payload, begin.segmentName);
    return payload;
}

bool DecodePreviewBeginBatch (const uint8_t* bytes, size_t size, PreviewBeginBatchPayload& begin, std::string& error)
{
    if (bytes == nullptr || size < 20) {
        error = "The preview batch header was short.";
        return false;
    }

    const uint32_t primitiveCount = ReadUInt32 (bytes + 8);
    const uint32_t segmentBytes = ReadUInt32 (bytes + 12);
    const uint32_t nameBytes = ReadUInt32 (bytes + 16);

    // ⚠️ CEILINGS BEFORE ALLOCATION, NOT AFTER. A definition previewing millions
    // of primitives is the case this defends against, and the host has no other
    // defence against a process it does not trust.
    if (primitiveCount > MaxPreviewPrimitivesPerBatch) {
        error = "The preview batch claimed " + std::to_string (primitiveCount) + " primitives, over the " +
                std::to_string (MaxPreviewPrimitivesPerBatch) + " limit.";
        return false;
    }
    if (segmentBytes > MaxPreviewSegmentBytes) {
        error = "The preview batch claimed a " + std::to_string (segmentBytes) + "-byte segment, over the " +
                std::to_string (MaxPreviewSegmentBytes) + "-byte limit.";
        return false;
    }
    if (nameBytes > MaxPreviewSegmentNameBytes) {
        error = "The preview batch's segment name claimed " + std::to_string (nameBytes) + " bytes, over the " +
                std::to_string (MaxPreviewSegmentNameBytes) + "-byte limit.";
        return false;
    }
    // Reconciled against what ARRIVED, not against itself: the frame's
    // payloadBytes is the only authority for how much there is.
    if ((uint64_t) nameBytes + 20ull != (uint64_t) size) {
        error = "The preview batch's declared segment name does not match its payload.";
        return false;
    }

    std::string name ((const char*) bytes + 20, nameBytes);
    if (ContainsNul (name)) {
        error = "The preview batch's segment name contained an embedded NUL.";
        return false;
    }
    // A segment nobody can open is worse than no segment: the primitives that
    // reference it would each fail separately, one line per primitive.
    if (segmentBytes > 0u && name.empty ()) {
        error = "The preview batch declared a segment but no name for it.";
        return false;
    }

    begin.epoch = ReadUInt32 (bytes);
    begin.revision = ReadUInt32 (bytes + 4);
    begin.primitiveCount = primitiveCount;
    begin.segmentBytes = segmentBytes;
    begin.segmentName = name;
    return true;
}

std::vector<uint8_t> EncodePreviewEndBatch (const PreviewEndBatchPayload& end)
{
    std::vector<uint8_t> payload;
    payload.reserve (20);
    AppendUInt32 (payload, end.epoch);
    AppendUInt32 (payload, end.revision);
    AppendUInt32 (payload, end.entryCount);
    AppendUInt64 (payload, end.checksum);
    return payload;
}

bool DecodePreviewEndBatch (const uint8_t* bytes, size_t size, PreviewEndBatchPayload& end, std::string& error)
{
    if (bytes == nullptr || size != 20) {
        error = "The preview batch footer was not 20 bytes.";
        return false;
    }

    const uint32_t entryCount = ReadUInt32 (bytes + 8);
    if (entryCount > MaxPreviewPrimitivesPerBatch) {
        error = "The preview batch footer claimed " + std::to_string (entryCount) + " entries, over the " +
                std::to_string (MaxPreviewPrimitivesPerBatch) + " limit.";
        return false;
    }

    end.epoch = ReadUInt32 (bytes);
    end.revision = ReadUInt32 (bytes + 4);
    end.entryCount = entryCount;
    end.checksum = ReadUInt64 (bytes + 12);
    return true;
}

std::vector<uint8_t> EncodePreviewIdRun (const PreviewIdRunPayload& run, bool withFlags)
{
    std::vector<uint8_t> payload;
    payload.reserve (16 + run.ids.size () * 8);
    AppendUInt32 (payload, run.epoch);
    AppendUInt32 (payload, run.revision);
    AppendUInt32 (payload, (uint32_t) run.ids.size ());
    if (withFlags) {
        payload.push_back (run.flagValue);
        payload.push_back (run.flagMask);
        AppendUInt16 (payload, 0);
    }
    for (size_t index = 0; index < run.ids.size (); ++index)
        AppendUInt64 (payload, run.ids[index]);
    return payload;
}

bool DecodePreviewIdRun (const uint8_t* bytes, size_t size, bool withFlags, PreviewIdRunPayload& run,
                         std::string& error)
{
    const size_t fixed = withFlags ? 16u : 12u;
    if (bytes == nullptr || size < fixed) {
        error = "The preview id run was short.";
        return false;
    }

    const uint32_t count = ReadUInt32 (bytes + 8);
    if (count > MaxPreviewIdsPerRun) {
        error = "The preview id run claimed " + std::to_string (count) + " ids, over the " +
                std::to_string (MaxPreviewIdsPerRun) + " limit.";
        return false;
    }
    if ((uint64_t) count * 8ull + (uint64_t) fixed != (uint64_t) size) {
        error = "The preview id run's declared count does not match its payload.";
        return false;
    }

    uint8_t flagValue = 0;
    uint8_t flagMask = 0;
    if (withFlags) {
        flagValue = bytes[12];
        flagMask = bytes[13];
        // A flag message that speaks for nothing would be applied as a no-op and
        // still cost a cache swap, so it is a malformed message rather than an
        // empty one.
        if (flagMask == 0u) {
            error = "The preview flag run named no flags to change.";
            return false;
        }
        if ((flagValue & ~flagMask) != 0u) {
            error = "The preview flag run set bits outside the flags it speaks for.";
            return false;
        }
    }

    std::vector<uint64_t> ids;
    ids.reserve (count);
    for (uint32_t index = 0; index < count; ++index)
        ids.push_back (ReadUInt64 (bytes + fixed + (size_t) index * 8));

    run.epoch = ReadUInt32 (bytes);
    run.revision = ReadUInt32 (bytes + 4);
    run.flagValue = flagValue;
    run.flagMask = flagMask;
    run.ids.swap (ids);
    return true;
}

std::vector<uint8_t> EncodePreviewDropAll (const PreviewDropAllPayload& drop)
{
    std::vector<uint8_t> payload;
    payload.reserve (8 + drop.reason.size ());
    AppendUInt32 (payload, drop.epoch);
    AppendUInt32 (payload, (uint32_t) drop.reason.size ());
    AppendText (payload, drop.reason);
    return payload;
}

bool DecodePreviewDropAll (const uint8_t* bytes, size_t size, PreviewDropAllPayload& drop, std::string& error)
{
    if (bytes == nullptr || size < 8) {
        error = "The preview drop-all was short.";
        return false;
    }

    const uint32_t reasonBytes = ReadUInt32 (bytes + 4);
    if ((uint64_t) reasonBytes + 8ull != (uint64_t) size) {
        error = "The preview drop-all's declared reason does not match its payload.";
        return false;
    }

    std::string reason ((const char*) bytes + 8, reasonBytes);
    if (ContainsNul (reason)) {
        error = "The preview drop-all's reason contained an embedded NUL.";
        return false;
    }

    drop.epoch = ReadUInt32 (bytes);
    drop.reason = reason;
    return true;
}

std::vector<uint8_t> EncodePreviewBatchAck (const PreviewBatchAckPayload& ack)
{
    std::vector<uint8_t> payload;
    payload.reserve (16 + ack.reason.size ());
    AppendUInt32 (payload, ack.epoch);
    AppendUInt32 (payload, ack.revision);
    AppendUInt32 (payload, ack.accepted ? 1u : 0u);
    AppendUInt32 (payload, (uint32_t) ack.reason.size ());
    AppendText (payload, ack.reason);
    return payload;
}

bool DecodePreviewBatchAck (const uint8_t* bytes, size_t size, PreviewBatchAckPayload& ack, std::string& error)
{
    if (bytes == nullptr || size < 16) {
        error = "The preview batch ack was short.";
        return false;
    }

    const uint32_t accepted = ReadUInt32 (bytes + 8);
    if (accepted > 1u) {
        error = "The preview batch ack's verdict was neither accepted nor refused.";
        return false;
    }

    const uint32_t reasonBytes = ReadUInt32 (bytes + 12);
    if ((uint64_t) reasonBytes + 16ull != (uint64_t) size) {
        error = "The preview batch ack's declared reason does not match its payload.";
        return false;
    }

    std::string reason ((const char*) bytes + 16, reasonBytes);
    if (ContainsNul (reason)) {
        error = "The preview batch ack's reason contained an embedded NUL.";
        return false;
    }
    // A refusal with no reason is what makes a preview that silently stops
    // working unexplainable, so it is refused as a message.
    if (accepted == 0u && reason.empty ()) {
        error = "The preview batch ack refused a batch without naming a reason.";
        return false;
    }

    ack.epoch = ReadUInt32 (bytes);
    ack.revision = ReadUInt32 (bytes + 4);
    ack.accepted = accepted == 1u;
    ack.reason = reason;
    return true;
}

std::vector<uint8_t> EncodePreviewResyncRequest (const PreviewResyncRequestPayload& request)
{
    std::vector<uint8_t> payload;
    payload.reserve (8 + request.reason.size ());
    AppendUInt32 (payload, request.epoch);
    AppendUInt32 (payload, (uint32_t) request.reason.size ());
    AppendText (payload, request.reason);
    return payload;
}

bool DecodePreviewResyncRequest (const uint8_t* bytes, size_t size, PreviewResyncRequestPayload& request,
                                 std::string& error)
{
    PreviewDropAllPayload shaped;
    if (!DecodePreviewDropAll (bytes, size, shaped, error))
        return false;
    if (shaped.reason.empty ()) {
        error = "The preview resync request named no reason.";
        return false;
    }
    request.epoch = shaped.epoch;
    request.reason = shaped.reason;
    return true;
}

std::vector<uint8_t> EncodePreviewPicked (const PreviewPickedPayload& picked)
{
    std::vector<uint8_t> payload;
    payload.reserve (8);
    AppendUInt64 (payload, picked.primitiveId);
    return payload;
}

bool DecodePreviewPicked (const uint8_t* bytes, size_t size, PreviewPickedPayload& picked, std::string& error)
{
    if (bytes == nullptr || size != 8) {
        error = "The preview pick was not 8 bytes.";
        return false;
    }
    const uint64_t id = ReadUInt64 (bytes);
    if (id == 0u) {
        error = "The preview pick carried no primitive id.";
        return false;
    }
    picked.primitiveId = id;
    return true;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

namespace {

void AppendPrimitiveHeader (std::vector<uint8_t>& buffer, const PreviewPrimitiveHeader& header)
{
    AppendUInt64 (buffer, header.primitiveId);
    buffer.push_back ((uint8_t) header.kind);
    buffer.push_back (header.flags);
    buffer.push_back ((uint8_t) header.surface);
    buffer.push_back (header.reserved);
    AppendUInt32 (buffer, header.itemIndex);
    buffer.insert (buffer.end (), header.componentGuid, header.componentGuid + 16);
    buffer.insert (buffer.end (), header.parameterGuid, header.parameterGuid + 16);
    AppendUInt32 (buffer, header.branchHash);
    AppendUInt64 (buffer, header.contentHash);
    AppendUInt32 (buffer, header.revision);
    AppendUInt32 (buffer, header.payloadBytes);
    AppendUInt64 (buffer, header.segmentOffset);
    AppendUInt32 (buffer, header.segmentBytes);
}

void AppendDescriptor (std::vector<uint8_t>& buffer, const PreviewPayloadDescriptor& descriptor)
{
    AppendUInt32 (buffer, descriptor.positionFloats);
    AppendUInt32 (buffer, descriptor.normalFloats);
    AppendUInt32 (buffer, descriptor.indexCount);
    AppendUInt32 (buffer, descriptor.textBytes);
    buffer.push_back (descriptor.closed ? 1u : 0u);
    buffer.push_back (0);
    AppendUInt16 (buffer, 0);
}

} // namespace

std::vector<uint8_t> EncodePreviewPrimitive (const PreviewPrimitiveMessage& message)
{
    PreviewPrimitiveHeader header = message.header;
    header.payloadBytes = PreviewDescriptorSize;
    header.segmentBytes = 0;
    if (message.inSegment) {
        header.segmentBytes = (uint32_t) ArrayBytes (message.descriptor);
        header.segmentOffset = message.header.segmentOffset;
    }
    else {
        header.payloadBytes = (uint32_t) (PreviewDescriptorSize + ArrayBytes (message.descriptor));
        header.segmentOffset = 0;
    }

    std::vector<uint8_t> buffer;
    buffer.reserve (PreviewHeaderSize + header.payloadBytes);
    AppendPrimitiveHeader (buffer, header);
    AppendDescriptor (buffer, message.descriptor);
    if (message.inSegment)
        return buffer;

    for (size_t index = 0; index < message.positions.size (); ++index)
        AppendFloat (buffer, message.positions[index]);
    for (size_t index = 0; index < message.normals.size (); ++index)
        AppendFloat (buffer, message.normals[index]);
    for (size_t index = 0; index < message.indices.size (); ++index)
        AppendUInt32 (buffer, message.indices[index]);
    AppendText (buffer, message.text);
    return buffer;
}

bool DecodePreviewPrimitive (const uint8_t* bytes, size_t size, PreviewPrimitiveMessage& message, std::string& error)
{
    if (bytes == nullptr || size < PreviewHeaderSize + PreviewDescriptorSize) {
        error = "The preview primitive was shorter than its header and descriptor.";
        return false;
    }

    const uint8_t kindValue = bytes[8];
    // ⚠️ THE KIND IS CHECKED BEFORE ANYTHING IS READ THROUGH IT. Everything
    // below -- the per-kind rules, the host's draw path, the pick range -- is
    // keyed on it, and an unknown kind must be a refusal rather than a default.
    if (!KnownPreviewKind (kindValue)) {
        error =
            "The preview primitive kind " + std::to_string ((uint32_t) kindValue) + " is not one this add-on knows.";
        return false;
    }

    // ⚠️ A SURFACE THE HOST DOES NOT KNOW IS A REFUSAL, NOT A DEFAULT. Falling
    // back to the 3D window would draw plan linework in the model, which reads
    // as a bug in the definition rather than as a version mismatch between the
    // two halves.
    const uint8_t surfaceValue = bytes[10];
    if (!KnownPreviewSurface (surfaceValue)) {
        error = "The preview primitive named drawing surface " + std::to_string ((uint32_t) surfaceValue) +
                ", which is not one this add-on knows.";
        return false;
    }

    PreviewPrimitiveHeader header;
    header.primitiveId = ReadUInt64 (bytes);
    header.kind = (PreviewKind) kindValue;
    header.flags = bytes[9];
    header.surface = (PreviewSurface) bytes[10];
    header.reserved = bytes[11];
    header.itemIndex = ReadUInt32 (bytes + 12);
    std::memcpy (header.componentGuid, bytes + 16, 16);
    std::memcpy (header.parameterGuid, bytes + 32, 16);
    header.branchHash = ReadUInt32 (bytes + 48);
    header.contentHash = ReadUInt64 (bytes + 52);
    header.revision = ReadUInt32 (bytes + 60);
    header.payloadBytes = ReadUInt32 (bytes + 64);
    header.segmentOffset = ReadUInt64 (bytes + 68);
    header.segmentBytes = ReadUInt32 (bytes + 76);

    if (header.primitiveId == 0u) {
        error = "The preview primitive carried no id.";
        return false;
    }
    if (header.payloadBytes < PreviewDescriptorSize) {
        error = "The preview primitive's payload was too small to hold its descriptor.";
        return false;
    }
    if (header.payloadBytes > MaxPreviewInlinePayloadBytes) {
        error = "The preview primitive's payload claimed " + std::to_string (header.payloadBytes) +
                " bytes, over the " + std::to_string (MaxPreviewInlinePayloadBytes) + "-byte limit.";
        return false;
    }
    if ((uint64_t) PreviewHeaderSize + (uint64_t) header.payloadBytes != (uint64_t) size) {
        error = "The preview primitive's declared payload does not match what arrived.";
        return false;
    }

    PreviewPayloadDescriptor descriptor;
    descriptor.positionFloats = ReadUInt32 (bytes + PreviewHeaderSize);
    descriptor.normalFloats = ReadUInt32 (bytes + PreviewHeaderSize + 4);
    descriptor.indexCount = ReadUInt32 (bytes + PreviewHeaderSize + 8);
    descriptor.textBytes = ReadUInt32 (bytes + PreviewHeaderSize + 12);
    descriptor.closed = bytes[PreviewHeaderSize + 16] != 0;

    if (!ValidatePreviewPayload (header.kind, descriptor, error))
        return false;

    const uint64_t arrayBytes = ArrayBytes (descriptor);
    const bool inSegment = header.segmentBytes != 0u;

    if (inSegment) {
        // The descriptor travels on the pipe; the arrays do not. What is checked
        // HERE is that the two halves agree about how much there is; whether the
        // segment actually holds it is ReadPreviewSegment's question, and it is
        // asked against the segment's own declared size.
        if (header.payloadBytes != PreviewDescriptorSize) {
            error = "The preview primitive declared a segment and inline arrays at the same time.";
            return false;
        }
        if (arrayBytes != (uint64_t) header.segmentBytes) {
            error = "The preview primitive's segment length does not match the arrays it declared.";
            return false;
        }
        if (arrayBytes == 0u) {
            error = "The preview primitive declared an empty segment.";
            return false;
        }
    }
    else {
        if ((uint64_t) PreviewDescriptorSize + arrayBytes != (uint64_t) header.payloadBytes) {
            error = "The preview primitive's declared arrays do not match its payload.";
            return false;
        }
    }

    message.header = header;
    message.descriptor = descriptor;
    message.inSegment = inSegment;
    message.positions.clear ();
    message.normals.clear ();
    message.indices.clear ();
    message.text.clear ();
    if (inSegment)
        return true;

    const uint8_t* cursor = bytes + PreviewHeaderSize + PreviewDescriptorSize;
    message.positions.reserve (descriptor.positionFloats);
    for (uint32_t index = 0; index < descriptor.positionFloats; ++index)
        message.positions.push_back (ReadFloat (cursor + (size_t) index * 4));
    cursor += (size_t) descriptor.positionFloats * 4;

    message.normals.reserve (descriptor.normalFloats);
    for (uint32_t index = 0; index < descriptor.normalFloats; ++index)
        message.normals.push_back (ReadFloat (cursor + (size_t) index * 4));
    cursor += (size_t) descriptor.normalFloats * 4;

    message.indices.reserve (descriptor.indexCount);
    for (uint32_t index = 0; index < descriptor.indexCount; ++index)
        message.indices.push_back (ReadUInt32 (cursor + (size_t) index * 4));
    cursor += (size_t) descriptor.indexCount * 4;

    message.text.assign ((const char*) cursor, descriptor.textBytes);
    if (ContainsNul (message.text)) {
        error = "The preview primitive's text contained an embedded NUL.";
        return false;
    }

    return ValidatePreviewIndices (message, error);
}

bool ReadPreviewSegment (const uint8_t* segment, size_t segmentSize, PreviewPrimitiveMessage& message,
                         std::string& error)
{
    if (!message.inSegment) {
        error = "The preview primitive's arrays were inline; there is no segment to read.";
        return false;
    }
    if (segment == nullptr) {
        error = "The preview batch's segment was not mapped.";
        return false;
    }

    const uint64_t offset = message.header.segmentOffset;
    const uint64_t length = (uint64_t) message.header.segmentBytes;

    // ⚠️ EVERY OFFSET AND LENGTH AGAINST THE DECLARED SIZE, BEFORE A SINGLE BYTE
    // IS READ, AND IN 64 BITS SO THE SUM CANNOT WRAP. A malformed worker must
    // produce a refused batch and a log line, never an out-of-bounds read inside
    // Archicad.exe.
    if (offset > (uint64_t) segmentSize || length > (uint64_t) segmentSize - offset) {
        error = "The preview primitive's segment range (" + std::to_string (offset) + " + " + std::to_string (length) +
                ") lies outside the " + std::to_string (segmentSize) + "-byte segment.";
        return false;
    }

    const PreviewPayloadDescriptor& descriptor = message.descriptor;
    if (ArrayBytes (descriptor) != length) {
        error = "The preview primitive's segment length does not match the arrays it declared.";
        return false;
    }

    const uint8_t* cursor = segment + offset;
    std::vector<float> positions;
    positions.reserve (descriptor.positionFloats);
    for (uint32_t index = 0; index < descriptor.positionFloats; ++index)
        positions.push_back (ReadFloat (cursor + (size_t) index * 4));
    cursor += (size_t) descriptor.positionFloats * 4;

    std::vector<float> normals;
    normals.reserve (descriptor.normalFloats);
    for (uint32_t index = 0; index < descriptor.normalFloats; ++index)
        normals.push_back (ReadFloat (cursor + (size_t) index * 4));
    cursor += (size_t) descriptor.normalFloats * 4;

    std::vector<uint32_t> indices;
    indices.reserve (descriptor.indexCount);
    for (uint32_t index = 0; index < descriptor.indexCount; ++index)
        indices.push_back (ReadUInt32 (cursor + (size_t) index * 4));
    cursor += (size_t) descriptor.indexCount * 4;

    std::string text ((const char*) cursor, descriptor.textBytes);
    if (ContainsNul (text)) {
        error = "The preview primitive's text contained an embedded NUL.";
        return false;
    }

    message.positions.swap (positions);
    message.normals.swap (normals);
    message.indices.swap (indices);
    message.text.swap (text);
    // Copied OUT, so the mapped view can be unmapped and the batch acknowledged.
    // The host never hands a pointer into the worker's memory to the render
    // thread.
    message.inSegment = false;
    return ValidatePreviewIndices (message, error);
}

} // namespace protocol
} // namespace grasshopper
} // namespace evp
