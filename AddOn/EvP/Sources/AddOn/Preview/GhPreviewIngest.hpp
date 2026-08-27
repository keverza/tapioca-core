#ifndef EVP_PREVIEW_GHPREVIEWINGEST_HPP
#define EVP_PREVIEW_GHPREVIEWINGEST_HPP

// What the host DOES with a preview message once the codec has decoded it.
//
// GhPreviewProtocol.cpp decides whether bytes are well formed. GhPreviewCache
// decides what a well-formed message means for the mirror. This file is the
// third thing, and it is the one the transport actually calls: the ORDER rules
// (nothing before a BeginBatch, one batch at a time), the CAPABILITY gate, the
// segment's lifetime, and what has to be written back down the pipe -- an ack
// that releases the worker's segment, or a resync request that says the next
// batch must be a full one.
//
// It lives here rather than in GhBridge for the reason every other rule in this
// pipeline lives outside the transport: the transport is a Win32 named pipe on
// an IO thread and cannot be run in a test, and every interesting failure on
// this path is a SEQUENCE failure -- a batch abandoned mid-flight, a segment
// that could not be mapped, an Added with no batch open, a checksum that
// disagrees. Those are exactly what an offline test can pin.
//
// ⚠️ DevKit-free, Win32-free, ACAPI-free, GPU-free, like GhPreviewCache beside
// it. The shared-memory segment is reached through GhPreviewSegmentSource below,
// which is an interface precisely so that this file does not name a Win32 type
// and the tests can hand it a buffer.
//
// ⚠️ THE PRODUCER IS THE UNTRUSTED SIDE. Every refusal below is a NAMED reason
// that travels back to the worker and into grasshopper.log. A preview that
// silently does not appear is the worst outcome on this path: the author has no
// way to tell "my definition produced nothing" from "the host threw my batch
// away", and would debug the wrong half.

#include "Grasshopper/GhProtocol.hpp"
#include "Grasshopper/GhPreviewProtocol.hpp"
#include "Preview/GhPreviewCache.hpp"

#include <cstdint>
#include <string>

namespace evp::preview {

// The batch's shared-memory segment, as the ingest sees it: something that can
// be opened by name, read within a known size, and closed.
//
// ⚠️ THE HOST MAPS IT READ-ONLY AND COPIES OUT BEFORE ACKNOWLEDGING. It never
// holds a view across frames and never hands a mapped pointer to the render
// thread. A producer that can rewrite the memory a render thread is reading is
// the same class of bug SceneCmdQueue's header documents, one process boundary
// further out -- and here the producer is a process running third-party
// components, so it is not a hypothetical.
class GhPreviewSegmentSource {
  public:
    virtual ~GhPreviewSegmentSource () = default;

    // Maps the named segment read-only. `declaredBytes` is what the batch said
    // it would be; an implementation MUST refuse a mapping shorter than that
    // rather than trusting the name.
    virtual bool Open (const std::string& name, uint32_t declaredBytes, std::string& error) = 0;

    // Null when nothing is open.
    virtual const uint8_t* Data () const = 0;
    virtual std::size_t Size () const = 0;

    // Idempotent. Called on every path out of a batch, including the failures.
    virtual void Close () = 0;
};

// What the transport must WRITE BACK after handing a message in. Returned by
// value rather than sent from here, because sending is the pipe's job and this
// file does not know what a pipe is.
struct GhPreviewReply {
    // Sent at the end of every batch, accepted or not. It is what RELEASES the
    // worker's segment: the worker holds the mapping alive until this arrives,
    // so a host that forgot to ack would leak the worker's address space one
    // batch at a time.
    bool sendAck = false;
    evp::grasshopper::protocol::PreviewBatchAckPayload ack;

    // "Do not send me another delta against a cache neither of us can vouch
    // for." Sent on a checksum disagreement and on any refusal that abandoned a
    // batch.
    bool sendResync = false;
    evp::grasshopper::protocol::PreviewResyncRequestPayload resync;

    // One line for grasshopper.log, or empty. Never merged with the reason on
    // the wire: the worker gets the short reason, the log gets the context.
    std::string log;
};

// The prefix every batch segment's name must carry. Both halves write it (see
// PreviewChannel.EncodeBatch) and the host checks it.
constexpr const char* PreviewSegmentNamePrefix = "Tapioca.GhPreview.";

// ⚠️ THE HOST DOES NOT OPEN A KERNEL OBJECT BY A NAME THE WORKER CHOSE FREELY.
// The name arrives from a process running third-party components, and it goes
// straight into OpenFileMapping inside Archicad.exe: a name carrying a namespace
// prefix ("Global\\...") or a path separator would let a batch point the host at
// somebody else's section object and then read it as preview geometry. So the
// name is constrained to this add-on's own prefix and to characters that cannot
// change which namespace is addressed.
//
// Pure, and here rather than beside the Win32 mapping, because "which names are
// acceptable" is exactly the kind of rule that must be pinned by a test rather
// than by reading the call site.
bool AcceptablePreviewSegmentName (const std::string& name, std::string& error);

class GhPreviewIngest {
  public:
    GhPreviewIngest (GhPreviewCache& cache, GhPreviewSegmentSource& segment);

    // The host's half of the handshake result. Called once, with the word the
    // host GRANTED (not the one the worker offered).
    //
    // ⚠️ OFF MUST COST NOTHING, AND THIS IS ONLY THE SECOND LINE OF DEFENCE. A
    // worker told at the handshake that preview was not granted does not
    // collect, convert or send, so nothing should ever reach here. If something
    // does, it is refused rather than dropped: silently ignoring it would hide a
    // worker whose build disagrees with this one.
    void GrantCapabilities (uint32_t granted);
    bool PreviewGranted () const;

    // The single entry point. `type` must be one of the Preview* message types;
    // anything else is refused as a programming error in the caller rather than
    // guessed at.
    GhPreviewReply OnMessage (evp::grasshopper::protocol::MessageType type, const uint8_t* bytes, std::size_t size);

    // The worker died, the bridge dropped, or Archicad is shutting down. Any
    // staged batch is discarded whole and the segment is closed -- a segment
    // belonging to a process that no longer exists must not stay mapped, and
    // half a batch on screen is a picture of a building that never existed.
    void OnWorkerGone (const std::string& reason);

    // True between a BeginBatch and its EndBatch, whether or not the batch has
    // already been refused.
    bool BatchOpen () const;

  private:
    // Every refusal takes this one path: abandon what was staged, remember that
    // the batch is spoiled so the rest of it is neither applied nor refused
    // again, and carry the reason forward to the ack at EndBatch.
    //
    // `needsResync` is false for the one refusal that is not the worker's fault
    // -- a batch from a stale epoch, which is what a restart looks like from
    // here. Asking that worker for a full resync would be asking it to resend a
    // generation nobody wants.
    GhPreviewReply Spoil (const std::string& reason, bool needsResync);
    void CloseBatch ();

    GhPreviewCache& cache;
    GhPreviewSegmentSource& segment;

    uint32_t granted = 0;
    bool batchOpen = false;
    // Set the moment anything in a batch is refused. The remaining messages of
    // that batch are then ignored WITHOUT a second refusal each -- a malformed
    // batch of 200000 primitives must cost one reason, not 200000.
    bool batchSpoiled = false;
    // Whether the spoiled batch's ack should be followed by "send me a full one
    // next". Separate from `batchSpoiled` because a stale epoch spoils a batch
    // without anybody being wrong.
    bool batchNeedsResync = false;
    std::string spoiledReason;
    uint32_t batchEpoch = 0;
    uint32_t batchRevision = 0;
    bool segmentOpen = false;
};

} // namespace evp::preview

#endif
