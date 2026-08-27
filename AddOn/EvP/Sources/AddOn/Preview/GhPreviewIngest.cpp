#include "Preview/GhPreviewIngest.hpp"

namespace evp::preview {

using namespace evp::grasshopper::protocol;

bool AcceptablePreviewSegmentName (const std::string& name, std::string& error)
{
    const std::string prefix (PreviewSegmentNamePrefix);
    if (name.size () <= prefix.size () || name.compare (0, prefix.size (), prefix) != 0) {
        error = "A preview batch named its segment \"" + name + "\", which is not a name this add-on issues.";
        return false;
    }
    if (name.size () > MaxPreviewSegmentNameBytes) {
        error = "A preview batch's segment name was " + std::to_string (name.size ()) + " bytes, over the " +
                std::to_string (MaxPreviewSegmentNameBytes) + "-byte limit.";
        return false;
    }

    for (const char character : name) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        const bool punctuation = character == '.' || character == '_' || character == '-';
        if (!digit && !lower && !upper && !punctuation) {
            // Named rather than merely refused: a backslash here is the whole
            // reason the rule exists, and "invalid name" would send whoever
            // hits it looking at the wrong half.
            error = "A preview batch's segment name contained a character this add-on will not open a shared-memory "
                    "section by. Only letters, digits, '.', '_' and '-' are accepted, so that no batch can address a "
                    "different object namespace.";
            return false;
        }
    }
    return true;
}

GhPreviewIngest::GhPreviewIngest (GhPreviewCache& cacheRef, GhPreviewSegmentSource& segmentRef)
    : cache (cacheRef), segment (segmentRef)
{
}

void GhPreviewIngest::GrantCapabilities (uint32_t grantedBits)
{
    granted = grantedBits;
}

bool GhPreviewIngest::PreviewGranted () const
{
    return (granted & CapabilityPreview) != 0u;
}

bool GhPreviewIngest::BatchOpen () const
{
    return batchOpen;
}

void GhPreviewIngest::CloseBatch ()
{
    if (segmentOpen) {
        segment.Close ();
        segmentOpen = false;
    }
    batchOpen = false;
    batchSpoiled = false;
    batchNeedsResync = false;
    spoiledReason.clear ();
}

GhPreviewReply GhPreviewIngest::Spoil (const std::string& reason, bool needsResync)
{
    GhPreviewReply reply;

    // ⚠️ ONE REFUSAL PER BATCH, NOT ONE PER MESSAGE. A malformed batch of
    // 200000 primitives must cost a single reason on the wire and a single line
    // in the log; refusing each message separately would bury the FIRST reason,
    // which is the only one that says what actually went wrong.
    if (batchSpoiled)
        return reply;

    reply.log = reason;
    cache.AbandonBatch (reason);

    if (batchOpen) {
        batchSpoiled = true;
        batchNeedsResync = needsResync;
        spoiledReason = reason;
        // ⚠️ THE ACK WAITS FOR EndBatch, ALWAYS. An ack is keyed to
        // (epoch, revision) and is what RELEASES that batch's segment on the
        // worker; sending one while the worker is still writing the batch would
        // release memory it is still writing into.
        return reply;
    }

    // Outside a batch there is no ack to key and nothing to release, so the
    // only useful answer is "your next batch must be a full one".
    if (needsResync) {
        reply.sendResync = true;
        reply.resync.epoch = batchEpoch;
        reply.resync.reason = reason;
    }
    return reply;
}

void GhPreviewIngest::OnWorkerGone (const std::string& reason)
{
    // ⚠️ ABANDON, THEN DROP. The staged batch goes first because it was never
    // true; the live cache goes second because it belonged to a process that no
    // longer exists. Leaving either behind would draw a preview nothing can
    // update, remove or explain.
    cache.AbandonBatch (reason);
    cache.DropAll (0, reason);
    CloseBatch ();
    batchEpoch = 0;
    batchRevision = 0;
    acceptedSinceDrop = false;
}

GhPreviewReply GhPreviewIngest::OnMessage (MessageType type, const uint8_t* bytes, std::size_t size)
{
    GhPreviewReply reply;
    std::string error;

    if (!PreviewGranted ()) {
        // Not merely dropped. A worker sending preview it was not granted is a
        // worker whose build disagrees with this one, and saying so is the only
        // way that becomes visible.
        reply.log = std::string ("a worker sent a \"") + DescribeMessageType (type) +
                    "\" message without the preview capability the handshake granted";
        reply.sendResync = true;
        reply.resync.epoch = batchEpoch;
        reply.resync.reason = "This add-on did not grant the preview capability for this session.";
        return reply;
    }

    switch (type) {
        case MessageType::PreviewBeginBatch: {
            PreviewBeginBatchPayload begin;
            if (!DecodePreviewBeginBatch (bytes, size, begin, error))
                return Spoil (error, true);

            if (batchOpen)
                return Spoil ("A preview batch began while revision " + std::to_string (batchRevision) +
                                  " was still open.",
                              true);

            batchEpoch = begin.epoch;
            batchRevision = begin.revision;
            batchOpen = true;
            batchSpoiled = false;
            batchNeedsResync = false;
            spoiledReason.clear ();

            std::string reason;
            const GhPreviewApply applied = cache.BeginBatch (begin, reason);
            if (applied != GhPreviewApply::Applied) {
                // ⚠️ Ignored AND Refused SPOIL THE BATCH THE SAME WAY AND ANSWER
                // DIFFERENTLY. Ignored is a stale epoch: normal after a restart,
                // and asking for a resync would be asking a worker that has
                // already moved on to resend a generation nobody wants. Refused
                // is the worker's fault and the next batch has to be a full one.
                return Spoil (reason, applied == GhPreviewApply::Refused);
            }

            if (begin.segmentBytes > 0u) {
                if (begin.segmentName.empty ())
                    return Spoil ("A preview batch declared a " + std::to_string (begin.segmentBytes) +
                                      "-byte segment and no name to open it by.",
                                  true);
                if (!AcceptablePreviewSegmentName (begin.segmentName, error))
                    return Spoil (error, true);
                if (!segment.Open (begin.segmentName, begin.segmentBytes, error))
                    return Spoil (error, true);
                segmentOpen = true;
            }
            return reply;
        }

        case MessageType::PreviewAdded:
        case MessageType::PreviewChanged: {
            if (!batchOpen)
                return Spoil ("A preview primitive arrived outside a batch.", true);
            if (batchSpoiled)
                return reply;

            PreviewPrimitiveMessage message;
            if (!DecodePreviewPrimitive (bytes, size, message, error))
                return Spoil (error, true);

            if (message.inSegment) {
                if (!segmentOpen)
                    return Spoil ("A preview primitive referenced a segment the batch never declared.", true);
                // ⚠️ COPIED OUT HERE, BEFORE THE ACK AND BEFORE THE CACHE. What
                // reaches the cache is this process's own memory; nothing
                // downstream ever holds a pointer into the worker's.
                if (!ReadPreviewSegment (segment.Data (), segment.Size (), message, error))
                    return Spoil (error, true);
            }

            const PreviewChange change =
                type == MessageType::PreviewAdded ? PreviewChange::Added : PreviewChange::Changed;
            std::string reason;
            if (cache.Apply (message, change, reason) != GhPreviewApply::Applied)
                return Spoil (reason, true);
            return reply;
        }

        case MessageType::PreviewRemoved: {
            if (!batchOpen)
                return Spoil ("A preview removal arrived outside a batch.", true);
            if (batchSpoiled)
                return reply;

            PreviewIdRunPayload run;
            if (!DecodePreviewIdRun (bytes, size, false, run, error))
                return Spoil (error, true);

            std::string reason;
            const GhPreviewApply applied = cache.Remove (run, reason);
            if (applied == GhPreviewApply::Refused)
                return Spoil (reason, true);
            if (applied == GhPreviewApply::Ignored)
                reply.log = reason;
            return reply;
        }

        case MessageType::PreviewVisibility:
        case MessageType::PreviewSelection: {
            // ⚠️ SELECTION LEGITIMATELY ARRIVES BETWEEN BATCHES. Clicking a
            // component on the canvas does not re-solve, so there is no batch to
            // carry it; the cache applies it to the live map and publishes on
            // its own. Visibility usually rides inside a batch. Neither is
            // refused for the side it arrives on -- only the cache knows which
            // map to write, and it already does.
            if (batchOpen && batchSpoiled)
                return reply;

            PreviewIdRunPayload run;
            if (!DecodePreviewIdRun (bytes, size, true, run, error))
                return Spoil (error, true);

            std::string reason;
            const GhPreviewApply applied = cache.SetFlags (run, reason);
            if (applied == GhPreviewApply::Refused)
                return Spoil (reason, true);
            if (applied == GhPreviewApply::Ignored)
                reply.log = reason;
            return reply;
        }

        case MessageType::PreviewEndBatch: {
            PreviewEndBatchPayload end;
            if (!DecodePreviewEndBatch (bytes, size, end, error)) {
                // The footer is unreadable, so which batch it ended is unknown.
                // The open batch is abandoned and the worker is asked to resend;
                // no ack is sent, because acking a revision this side cannot
                // read would release the wrong segment.
                return Spoil (error, true);
            }

            if (!batchOpen) {
                // The worker IS waiting on an ack for this (epoch, revision) --
                // it is holding that batch's segment until one arrives -- so
                // this one case answers rather than staying silent.
                GhPreviewReply orphan;
                orphan.log = "A preview batch ended without having begun.";
                orphan.sendAck = true;
                orphan.ack.epoch = end.epoch;
                orphan.ack.revision = end.revision;
                orphan.ack.accepted = false;
                orphan.ack.reason = orphan.log;
                orphan.sendResync = true;
                orphan.resync.epoch = end.epoch;
                orphan.resync.reason = orphan.log;
                return orphan;
            }

            reply.sendAck = true;
            reply.ack.epoch = batchEpoch;
            reply.ack.revision = batchRevision;

            if (batchSpoiled) {
                reply.ack.accepted = false;
                reply.ack.reason = spoiledReason;
                reply.log = spoiledReason;
                if (batchNeedsResync) {
                    reply.sendResync = true;
                    reply.resync.epoch = batchEpoch;
                    reply.resync.reason = spoiledReason;
                }
                CloseBatch ();
                return reply;
            }

            const GhPreviewEndResult result = cache.EndBatch (end);
            if (result.apply == GhPreviewApply::Applied) {
                reply.ack.accepted = true;
                // The first batch of a generation says so. See the header: this
                // is the only positive evidence the whole path produces, and
                // without it a working transport and a dead one read the same.
                if (!acceptedSinceDrop) {
                    acceptedSinceDrop = true;
                    reply.log = "preview accepted: " + std::to_string (cache.Count ()) +
                                " primitive(s) held for epoch " + std::to_string (batchEpoch) +
                                ". Open Tapioca > Tapioca 3D Viewer to see them.";
                }
            }
            else {
                reply.ack.accepted = false;
                reply.ack.reason = result.reason;
                reply.log = result.reason;
                // Ignored here is a stale epoch and asks for nothing; Refused
                // and every checksum disagreement ask for a full batch next.
                if (result.apply == GhPreviewApply::Refused) {
                    reply.sendResync = true;
                    reply.resync.epoch = batchEpoch;
                    reply.resync.reason = result.reason;
                }
                cache.AbandonBatch (result.reason);
            }

            CloseBatch ();
            return reply;
        }

        case MessageType::PreviewDropAll: {
            PreviewDropAllPayload drop;
            if (!DecodePreviewDropAll (bytes, size, drop, error))
                return Spoil (error, true);

            // ⚠️ DROPALL IS NOT REFUSABLE AND IS NEVER ACKED. It is the worker
            // saying "everything you hold from me is gone" -- a closed
            // definition, or a fresh generation. There is nothing to disagree
            // with and nothing to release: a batch in flight dies with it, and
            // its segment belonged to the epoch being dropped.
            cache.AbandonBatch (drop.reason);
            cache.DropAll (drop.epoch, drop.reason);
            CloseBatch ();
            batchEpoch = drop.epoch;
            batchRevision = 0;
            acceptedSinceDrop = false;
            reply.log = drop.reason.empty () ? std::string ("the worker dropped its whole preview") : drop.reason;
            return reply;
        }

        default:
            // Host -> worker messages, and everything that is not preview at
            // all. Reaching here is a bug in the caller's routing rather than in
            // the worker, so it is recorded and not answered on the wire.
            reply.log = std::string ("preview ingest was handed a \"") + DescribeMessageType (type) +
                        "\" message, which is not one it serves";
            return reply;
    }
}

} // namespace evp::preview
