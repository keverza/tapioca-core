#include "Preview/GhPreviewCache.hpp"

#include <utility>

namespace evp::preview {

using namespace evp::grasshopper::protocol;

namespace {

std::shared_ptr<const GhPreviewPrimitive> Adopt (const PreviewPrimitiveMessage& message)
{
    auto primitive = std::make_shared<GhPreviewPrimitive> ();
    primitive->id = message.header.primitiveId;
    primitive->kind = message.header.kind;
    primitive->flags = message.header.flags;
    primitive->surface = message.header.surface;
    primitive->itemIndex = message.header.itemIndex;
    for (int index = 0; index < 16; ++index) {
        primitive->componentGuid[index] = message.header.componentGuid[index];
        primitive->parameterGuid[index] = message.header.parameterGuid[index];
    }
    primitive->branchHash = message.header.branchHash;
    primitive->contentHash = message.header.contentHash;
    primitive->revision = message.header.revision;
    primitive->closed = message.descriptor.closed;
    primitive->positions = message.positions;
    primitive->normals = message.normals;
    primitive->indices = message.indices;
    primitive->text = message.text;
    return primitive;
}

// Flags change, geometry does not. The buffers are shared with the old entry
// rather than copied, which is what makes a selection change over 500
// primitives cost the flag bytes and a pointer swap.
std::shared_ptr<const GhPreviewPrimitive> WithFlags (const std::shared_ptr<const GhPreviewPrimitive>& primitive,
                                                     uint8_t value, uint8_t mask)
{
    const uint8_t flags = (uint8_t) ((primitive->flags & ~mask) | (value & mask));
    if (flags == primitive->flags)
        return primitive;

    auto copy = std::make_shared<GhPreviewPrimitive> (*primitive);
    copy->flags = flags;
    return copy;
}

} // namespace

GhPreviewCache& GhPreviewCache::Get ()
{
    static GhPreviewCache instance;
    return instance;
}

void GhPreviewCache::DropAll (uint32_t newEpoch, const std::string&)
{
    std::lock_guard<std::mutex> guard (mutex);
    live.clear ();
    staged.clear ();
    batchOpen = false;
    batchEntries = 0;
    batchExpected = 0;
    batchChecksum = 0;
    batchRevision = 0;
    revision = 0;
    epoch = newEpoch;
    PublishLocked ();
}

GhPreviewApply GhPreviewCache::BeginBatch (const PreviewBeginBatchPayload& begin, std::string& reason)
{
    std::lock_guard<std::mutex> guard (mutex);

    // ⚠️ EPOCH FIRST. A batch from a generation that no longer exists is
    // dropped without complaint; it is not an error, it is what a restart looks
    // like from here. Zero is the "no worker has spoken yet" epoch and adopts
    // whatever arrives.
    if (epoch != 0u && begin.epoch != epoch) {
        reason = "A preview batch arrived from epoch " + std::to_string (begin.epoch) + "; this host holds epoch " +
                 std::to_string (epoch) + ".";
        return GhPreviewApply::Ignored;
    }
    if (batchOpen) {
        reason = "A preview batch began while revision " + std::to_string (batchRevision) + " was still open.";
        return GhPreviewApply::Refused;
    }
    // Monotonic within an epoch. A repeated or reversed revision means messages
    // were reordered or replayed, and applying it would leave the two caches
    // disagreeing in a way neither could detect afterwards.
    if (epoch != 0u && begin.revision <= revision) {
        reason = "A preview batch claimed revision " + std::to_string (begin.revision) + " after revision " +
                 std::to_string (revision) + ".";
        return GhPreviewApply::Refused;
    }
    if (begin.primitiveCount > MaxPreviewPrimitivesPerBatch) {
        reason = "A preview batch claimed " + std::to_string (begin.primitiveCount) + " primitives, over the " +
                 std::to_string (MaxPreviewPrimitivesPerBatch) + " limit.";
        return GhPreviewApply::Refused;
    }

    epoch = begin.epoch;
    // Staged from the LIVE map, so a batch that only changes one primitive
    // still publishes everything else unchanged -- and so that abandoning the
    // batch costs nothing but clearing this.
    staged = live;
    batchOpen = true;
    batchRevision = begin.revision;
    batchExpected = begin.primitiveCount;
    batchEntries = 0;
    batchChecksum = PreviewChecksumStart ();
    return GhPreviewApply::Applied;
}

GhPreviewApply GhPreviewCache::Apply (const PreviewPrimitiveMessage& message, PreviewChange change, std::string& reason)
{
    std::lock_guard<std::mutex> guard (mutex);

    if (!batchOpen) {
        reason = "A preview primitive arrived outside a batch.";
        return GhPreviewApply::Refused;
    }
    if (message.inSegment) {
        // The arrays are still in the worker's memory. Reading them is
        // ReadPreviewSegment's job and it happens BEFORE the ack that releases
        // the segment; the cache must never hold a reference into it.
        reason = "A preview primitive reached the cache with its arrays still in the worker's segment.";
        return GhPreviewApply::Refused;
    }
    if (message.header.revision != batchRevision) {
        reason = "A preview primitive claimed revision " + std::to_string (message.header.revision) +
                 " inside batch revision " + std::to_string (batchRevision) + ".";
        return GhPreviewApply::Refused;
    }
    if (batchEntries >= MaxPreviewPrimitivesPerBatch) {
        reason =
            "A preview batch exceeded the " + std::to_string (MaxPreviewPrimitivesPerBatch) + "-primitive ceiling.";
        return GhPreviewApply::Refused;
    }
    if (!ValidatePreviewPayload (message.header.kind, message.descriptor, reason))
        return GhPreviewApply::Refused;
    if (!ValidatePreviewIndices (message, reason))
        return GhPreviewApply::Refused;

    staged[message.header.primitiveId] = Adopt (message);
    batchChecksum = PreviewChecksumAccumulate (batchChecksum, message.header.primitiveId, change);
    ++batchEntries;
    return GhPreviewApply::Applied;
}

GhPreviewApply GhPreviewCache::Remove (const PreviewIdRunPayload& run, std::string& reason)
{
    std::lock_guard<std::mutex> guard (mutex);

    if (epoch != 0u && run.epoch != epoch) {
        reason = "A preview removal arrived from epoch " + std::to_string (run.epoch) + ".";
        return GhPreviewApply::Ignored;
    }
    if (!batchOpen) {
        reason = "A preview removal arrived outside a batch.";
        return GhPreviewApply::Refused;
    }
    if (run.revision != batchRevision) {
        reason = "A preview removal claimed revision " + std::to_string (run.revision) + " inside batch revision " +
                 std::to_string (batchRevision) + ".";
        return GhPreviewApply::Refused;
    }

    for (const uint64_t id : run.ids) {
        // An id the host does not hold is not an error. The worker's mirror is
        // authoritative and the two can legitimately disagree by one batch
        // after a resync; refusing here would turn a harmless disagreement into
        // a dropped batch.
        staged.erase (id);
        batchChecksum = PreviewChecksumAccumulate (batchChecksum, id, PreviewChange::Removed);
        ++batchEntries;
    }
    return GhPreviewApply::Applied;
}

GhPreviewApply GhPreviewCache::SetFlags (const PreviewIdRunPayload& run, std::string& reason)
{
    std::lock_guard<std::mutex> guard (mutex);

    if (epoch != 0u && run.epoch != epoch) {
        reason = "A preview flag change arrived from epoch " + std::to_string (run.epoch) + ".";
        return GhPreviewApply::Ignored;
    }
    if (run.flagMask == 0u) {
        reason = "A preview flag change named no flags.";
        return GhPreviewApply::Refused;
    }

    // ⚠️ SELECTION ARRIVES BETWEEN BATCHES, NOT INSIDE ONE. Clicking a component
    // on the canvas does not re-solve, so there is no batch to carry it: it is
    // applied to the live cache and published on its own. A visibility flag
    // inside a batch stages with the rest of it, which is why both paths exist.
    Map& target = batchOpen ? staged : live;
    for (const uint64_t id : run.ids) {
        auto found = target.find (id);
        if (found == target.end ())
            continue;
        found->second = WithFlags (found->second, run.flagValue, run.flagMask);
        if (batchOpen) {
            batchChecksum = PreviewChecksumAccumulate (batchChecksum, id, PreviewChange::Visibility);
            ++batchEntries;
        }
    }

    if (!batchOpen)
        PublishLocked ();
    return GhPreviewApply::Applied;
}

GhPreviewEndResult GhPreviewCache::EndBatch (const PreviewEndBatchPayload& end)
{
    std::lock_guard<std::mutex> guard (mutex);

    GhPreviewEndResult result;
    if (epoch != 0u && end.epoch != epoch) {
        result.apply = GhPreviewApply::Ignored;
        result.reason = "A preview batch ended from epoch " + std::to_string (end.epoch) + ".";
        return result;
    }
    if (!batchOpen) {
        result.apply = GhPreviewApply::Refused;
        result.reason = "A preview batch ended without having begun.";
        return result;
    }
    if (end.revision != batchRevision) {
        staged.clear ();
        batchOpen = false;
        result.apply = GhPreviewApply::Refused;
        result.resyncRequired = true;
        result.reason = "A preview batch ended at revision " + std::to_string (end.revision) + " having begun at " +
                        std::to_string (batchRevision) + ".";
        return result;
    }

    // ⚠️ THE CHECKSUM IS WHY A DROPPED MESSAGE DOES NOT BECOME A PERMANENTLY
    // WRONG VIEWPORT. The alternative -- trusting the arrival order -- fails
    // silently and stays failed until the next full resync, which may be never.
    // A disagreement costs one extra batch; drawing a cache neither side can
    // vouch for costs a user's confidence in the whole feature.
    if (end.checksum != batchChecksum) {
        staged.clear ();
        batchOpen = false;
        result.apply = GhPreviewApply::Refused;
        result.resyncRequired = true;
        result.reason = "A preview batch's ids did not sum to what its footer declared.";
        return result;
    }
    if (end.entryCount != batchEntries) {
        staged.clear ();
        batchOpen = false;
        result.apply = GhPreviewApply::Refused;
        result.resyncRequired = true;
        result.reason = "A preview batch declared " + std::to_string (end.entryCount) + " entries and carried " +
                        std::to_string (batchEntries) + ".";
        return result;
    }

    // The swap. One step, and it is the only place the live cache moves.
    live.swap (staged);
    staged.clear ();
    batchOpen = false;
    revision = batchRevision;
    PublishLocked ();

    result.apply = GhPreviewApply::Applied;
    return result;
}

void GhPreviewCache::AbandonBatch (const std::string&)
{
    std::lock_guard<std::mutex> guard (mutex);
    staged.clear ();
    batchOpen = false;
    batchEntries = 0;
    batchExpected = 0;
    batchChecksum = 0;
    // `live` and `revision` are untouched on purpose: an abandoned batch leaves
    // the viewport showing the last COMPLETE preview, which is the last picture
    // that was ever true.
}

std::shared_ptr<const GhPreviewSnapshot> GhPreviewCache::SnapshotCopy () const
{
    std::lock_guard<std::mutex> guard (mutex);
    return published;
}

uint32_t GhPreviewCache::Epoch () const
{
    std::lock_guard<std::mutex> guard (mutex);
    return epoch;
}

bool GhPreviewCache::BatchOpen () const
{
    std::lock_guard<std::mutex> guard (mutex);
    return batchOpen;
}

std::size_t GhPreviewCache::Count () const
{
    std::lock_guard<std::mutex> guard (mutex);
    return live.size ();
}

void GhPreviewCache::PublishLocked ()
{
    auto snapshot = std::make_shared<GhPreviewSnapshot> ();
    snapshot->generation = ++generation;
    snapshot->epoch = epoch;
    snapshot->revision = revision;
    snapshot->primitives.reserve (live.size ());
    for (const auto& entry : live)
        snapshot->primitives.push_back (entry.second);
    // Published objects never mutate, so a render host can keep this pointer
    // across frames without holding the store lock -- the same contract
    // RetainedPreviewStore makes.
    published = std::move (snapshot);
}

} // namespace evp::preview
