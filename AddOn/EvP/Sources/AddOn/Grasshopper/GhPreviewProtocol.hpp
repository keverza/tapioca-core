#ifndef EVP_GRASSHOPPER_GHPREVIEWPROTOCOL_HPP
#define EVP_GRASSHOPPER_GHPREVIEWPROTOCOL_HPP

// The preview half of the Archicad <-> Tapioca.GhWorker.exe wire.
//
// It is the SAME protocol as GhProtocol.hpp -- same 20-byte header, same
// version negotiation, same "no JSON, no text parsing on the control path" rule
// -- with message types added to it and a capability bit gating whether they
// may be used at all. It is a Version bump, not a second protocol, and this
// file is separate only because the payloads are big enough to be worth reading
// on their own.
//
// ⚠️ THIS CARRIES RESULTS, NOT DRAW CALLS. Nothing here mirrors
// Rhino.Display.DisplayPipeline. What crosses is what a definition MEANT -- a
// plane, an arrow, a mesh -- and everything view-dependent (gizmo axes, arrow
// heads, text billboarding, constant-width lines) is built host-side at the
// camera the viewport actually has. HANDOFF-GrasshopperInsideArchicad.md, "The
// key design decision", says why at length.
//
// ⚠️ THE PRODUCER IS THE UNTRUSTED SIDE HERE, WHICH IS THE OPPOSITE OF
// GhProtocol'S USUAL DIRECTION. Preview flows worker -> host, the worker is a
// separate process running third-party components, and its bytes end up
// indexing into memory inside Archicad.exe. So every count, offset and length
// below is validated against what actually ARRIVED before a single byte is
// read, and a malformed batch is a refusal with a named reason -- never an
// out-of-bounds read in the host.
//
// ⚠️ GEOMETRY CROSSES AS float32, DELIBERATELY. The GHA hashes doubles (see
// PreviewPrimitives.cs) so identity and change detection keep full precision;
// the WIRE carries floats because everything on this path is drawn and never
// measured. PREVIEW GEOMETRY IS NOT BIM GEOMETRY and must never become an
// import path -- element creation stays a deliberate Tapir or Tapioca write.
//
// DevKit-free, Win32-free and CLR-free, like GhProtocol.hpp and for the same
// reason: framing is where a transport goes wrong silently, and this is the
// half that can be proved offline.

#include <cstdint>
#include <string>
#include <vector>

namespace evp {
namespace grasshopper {
namespace protocol {

// The capability bit in HelloPayload.capabilities. A worker that does not set
// it is not asked for preview and must not send any of the messages below; a
// host with preview disabled clears it in the ack so the worker does not
// collect, convert or send. OFF MUST COST NOTHING -- not cost a transfer that
// is then dropped.
constexpr uint32_t CapabilityPreview = 1u << 0;

// The ten primitives, mirroring PreviewPrimitiveKind in
// Sources/GrasshopperComponents/PreviewPrimitives.cs.
//
// ⚠️ THESE NUMBERS ARE A WIRE CONTRACT AND NEITHER HALF INFERS THE OTHER'S.
// Renumbering one side is not a build error; it is a silent corruption that
// draws the wrong shape. Adding a kind is a version bump in both files.
enum class PreviewKind : uint8_t {
    TriangleMesh = 1,
    Polyline3D = 2,
    PointMarker = 3,
    PlaneGizmo = 4,
    Arrow3D = 5,
    BillboardText = 6,
    WorldText = 7,
    PointCloud = 8,
    BillboardSprite = 9,
    Bounds = 10,
};

// Mirrors PreviewFlags in PreviewPrimitives.cs.
constexpr uint8_t PreviewFlagVisible = 1u << 0;
constexpr uint8_t PreviewFlagSelected = 1u << 1;
constexpr uint8_t PreviewFlagHighlighted = 1u << 2;
constexpr uint8_t PreviewFlagXRay = 1u << 3;
constexpr uint8_t PreviewFlagDepthTest = 1u << 4;

// Mirrors PreviewChange in PreviewDelta.cs. It exists on this side only to
// reproduce the batch checksum byte for byte; the host never decides a change
// kind for itself.
enum class PreviewChange : uint8_t {
    Added = 0,
    Changed = 1,
    Removed = 2,
    Visibility = 3,
};

// The fixed-size primitive header, 80 bytes, little-endian POD.
//
// ⚠️ NO GH_Path TEXT, NO COMPONENT NICKNAMES, NO TYPE NAMES. They are
// variable-length UTF-8 in a message the host decodes per primitive, and the
// host does not need them to DRAW -- only to answer "what am I looking at",
// which is a correlated ApiRequest against branchHash, on demand, off the hot
// path.
constexpr uint32_t PreviewHeaderSize = 80;

struct PreviewPrimitiveHeader {
    // ⚠️ THE CACHE KEY, AND IT MUST NOT DEPEND ON CONTENT. It is
    // hash(componentGuid, parameterGuid, branchHash, itemIndex). If it depended
    // on content, every edit would read as a Remove plus an Add, the delta
    // protocol would degenerate into full retransmission, and selection state
    // would be lost on every solve. Content is what contentHash is for.
    uint64_t primitiveId = 0;
    PreviewKind kind = PreviewKind::Polyline3D;
    uint8_t flags = 0;
    uint16_t reserved = 0;
    uint32_t itemIndex = 0;
    uint8_t componentGuid[16] = {};
    uint8_t parameterGuid[16] = {};
    uint32_t branchHash = 0;
    uint64_t contentHash = 0;
    uint32_t revision = 0;
    // The INLINE payload: the descriptor below, plus the arrays when they are
    // small enough to travel on the pipe. Zero is not legal -- the descriptor
    // is always inline.
    uint32_t payloadBytes = 0;
    // The bulk, when there is any: an offset into the batch's shared-memory
    // segment. segmentBytes == 0 means everything is inline.
    uint64_t segmentOffset = 0;
    uint32_t segmentBytes = 0;
};

// What every primitive's payload starts with, inline, whatever its kind. 20
// bytes, and it is what tells the host how much to expect and where.
//
// One descriptor for all ten kinds on purpose: it gives the arrays exactly one
// validator instead of ten, and a per-kind rule table on top of it (see
// ValidatePreviewPayload) rather than ten hand-written parsers, which is where
// an over-read would otherwise hide.
constexpr uint32_t PreviewDescriptorSize = 20;

struct PreviewPayloadDescriptor {
    uint32_t positionFloats = 0; // multiple of 3
    uint32_t normalFloats = 0;   // 0, or equal to positionFloats
    uint32_t indexCount = 0;     // multiple of 3 for TriangleMesh, else 0
    uint32_t textBytes = 0;      // UTF-8, no NUL
    bool closed = false;         // Polyline3D only

    uint32_t PointCount () const
    {
        return positionFloats / 3;
    }
};

// A decoded primitive, arrays still where they were sent. `inSegment` says
// which: true and the arrays are in the batch segment at the header's offset,
// false and they follow the descriptor inline.
struct PreviewPrimitiveMessage {
    PreviewPrimitiveHeader header;
    PreviewPayloadDescriptor descriptor;
    bool inSegment = false;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    std::string text;
};

struct PreviewBeginBatchPayload {
    // Changes on every worker restart. A batch from a stale epoch is dropped
    // without complaint: after a kill and restart the host holds preview from a
    // process that no longer exists.
    uint32_t epoch = 0;
    // Monotonic within one epoch.
    uint32_t revision = 0;
    uint32_t primitiveCount = 0;
    uint32_t segmentBytes = 0;
    std::string segmentName; // UTF-8, empty when the batch carries no bulk
};

struct PreviewEndBatchPayload {
    uint32_t epoch = 0;
    uint32_t revision = 0;
    uint32_t entryCount = 0;
    // FNV-1a over (primitiveId, changeKind) in send order, computed by
    // PreviewBatch.Checksum() in PreviewDelta.cs. A host whose own sum
    // disagrees asks for a resync rather than drawing a cache it cannot trust.
    uint64_t checksum = 0;
};

// Removed / Visibility / Selection. Ids and flags, never geometry: toggling a
// component's preview off is a byte, not a retransmission.
struct PreviewIdRunPayload {
    uint32_t epoch = 0;
    uint32_t revision = 0;
    uint8_t flagValue = 0; // the bits to write
    uint8_t flagMask = 0;  // the bits this message speaks for; 0 for Removed
    std::vector<uint64_t> ids;
};

struct PreviewDropAllPayload {
    uint32_t epoch = 0;
    std::string reason; // UTF-8, for the log; may be empty
};

// host -> worker, the ack that RELEASES the batch's segment. The worker keeps
// the segment alive until this arrives, so a wedged host cannot leak the
// worker's address space -- GhWorkerHost's per-run timeout bounds the wait.
struct PreviewBatchAckPayload {
    uint32_t epoch = 0;
    uint32_t revision = 0;
    bool accepted = false;
    std::string reason; // named refusal when accepted is false
};

// host -> worker. Sent when a checksum disagrees or a message is refused: the
// next batch must be a full one rather than a delta against a cache neither
// side can vouch for.
struct PreviewResyncRequestPayload {
    uint32_t epoch = 0;
    std::string reason;
};

// host -> worker. A viewport pick resolved to a primitive; the worker selects
// and zooms to the owning component on the canvas.
//
// ⚠️ METADATA ONLY, BOTH DIRECTIONS, AND NEITHER MAY CAUSE A RE-SOLVE, A
// RETESSELLATION OR A GEOMETRY TRANSFER.
struct PreviewPickedPayload {
    uint64_t primitiveId = 0;
};

// Ceilings, checked BEFORE anything is allocated. The only defence available
// against a process Tapioca does not trust; a definition previewing millions of
// primitives must produce a named refusal and a truncated-preview line in the
// run report, not an allocation.
constexpr uint32_t MaxPreviewPrimitivesPerBatch = 200000;
constexpr uint32_t MaxPreviewSegmentBytes = 256u * 1024u * 1024u;
constexpr uint32_t MaxPreviewInlinePayloadBytes = 1024u * 1024u;
constexpr uint32_t MaxPreviewTextBytes = 4096;
constexpr uint32_t MaxPreviewIdsPerRun = 200000;
constexpr uint32_t MaxPreviewSegmentNameBytes = 256;

// ---------------------------------------------------------------------------
// Codec. Every Decode refuses rather than truncates, never partially fills its
// output on failure, and reports a reason a person can act on.
// ---------------------------------------------------------------------------

std::vector<uint8_t> EncodePreviewBeginBatch (const PreviewBeginBatchPayload& begin);
bool DecodePreviewBeginBatch (const uint8_t* bytes, size_t size, PreviewBeginBatchPayload& begin, std::string& error);

std::vector<uint8_t> EncodePreviewEndBatch (const PreviewEndBatchPayload& end);
bool DecodePreviewEndBatch (const uint8_t* bytes, size_t size, PreviewEndBatchPayload& end, std::string& error);

// `withFlags` distinguishes Removed (ids only) from Visibility/Selection (ids
// plus the flag bits they speak for).
std::vector<uint8_t> EncodePreviewIdRun (const PreviewIdRunPayload& run, bool withFlags);
bool DecodePreviewIdRun (const uint8_t* bytes, size_t size, bool withFlags, PreviewIdRunPayload& run,
                         std::string& error);

std::vector<uint8_t> EncodePreviewDropAll (const PreviewDropAllPayload& drop);
bool DecodePreviewDropAll (const uint8_t* bytes, size_t size, PreviewDropAllPayload& drop, std::string& error);

std::vector<uint8_t> EncodePreviewBatchAck (const PreviewBatchAckPayload& ack);
bool DecodePreviewBatchAck (const uint8_t* bytes, size_t size, PreviewBatchAckPayload& ack, std::string& error);

std::vector<uint8_t> EncodePreviewResyncRequest (const PreviewResyncRequestPayload& request);
bool DecodePreviewResyncRequest (const uint8_t* bytes, size_t size, PreviewResyncRequestPayload& request,
                                 std::string& error);

std::vector<uint8_t> EncodePreviewPicked (const PreviewPickedPayload& picked);
bool DecodePreviewPicked (const uint8_t* bytes, size_t size, PreviewPickedPayload& picked, std::string& error);

// Header + descriptor + (inline arrays, or nothing when they are in the
// segment). `message.inSegment` chooses.
std::vector<uint8_t> EncodePreviewPrimitive (const PreviewPrimitiveMessage& message);

// Decodes header and descriptor, and the inline arrays when there are any. A
// segment-resident primitive comes back with `inSegment` true and its arrays
// EMPTY -- reading the segment is ReadPreviewSegment's job, because that memory
// belongs to another process and needs the batch's declared size to be checked
// against.
bool DecodePreviewPrimitive (const uint8_t* bytes, size_t size, PreviewPrimitiveMessage& message, std::string& error);

// Copies the arrays out of a batch segment into `message`.
//
// ⚠️ THE HOST COPIES OUT BEFORE ACKNOWLEDGING AND NEVER HOLDS A VIEW ACROSS
// FRAMES. A producer that can rewrite the memory a render thread is reading is
// the same class of bug SceneCmdQueue's header documents, one process boundary
// further out. Every offset and length is validated against `segmentSize`
// first, and the counts against the bytes they claim.
bool ReadPreviewSegment (const uint8_t* segment, size_t segmentSize, PreviewPrimitiveMessage& message,
                         std::string& error);

// The per-kind rule table: how many points a kind may carry, whether it may
// carry normals, indices or text. Applied to inline and segment payloads
// alike, so a mesh cannot arrive claiming to be a plane.
bool ValidatePreviewPayload (PreviewKind kind, const PreviewPayloadDescriptor& descriptor, std::string& error);

// Indices are checked against the vertex count they will index into, wherever
// the arrays came from. An unchecked index is the one remaining way a
// malformed worker reaches memory it does not own.
bool ValidatePreviewIndices (const PreviewPrimitiveMessage& message, std::string& error);

// FNV-1a 64, byte for byte what PreviewHash in PreviewPrimitives.cs computes.
// The host recomputes no identity and no content hash; it reproduces the BATCH
// checksum, which is what PreviewEndBatch is compared against.
uint64_t PreviewChecksumStart ();
uint64_t PreviewChecksumAccumulate (uint64_t hash, uint64_t primitiveId, PreviewChange change);

const char* DescribePreviewKind (PreviewKind kind);

bool KnownPreviewKind (uint8_t value);

} // namespace protocol
} // namespace grasshopper
} // namespace evp

#endif
