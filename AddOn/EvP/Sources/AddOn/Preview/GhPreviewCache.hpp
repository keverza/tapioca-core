#ifndef EVP_PREVIEW_GHPREVIEWCACHE_HPP
#define EVP_PREVIEW_GHPREVIEWCACHE_HPP

// The host's mirror of what Grasshopper is previewing.
//
// The worker owns the diff (PreviewDelta.cs); this side owns nothing but what
// it was told. It never asks "what do you have" -- it applies Added, Changed,
// Removed, Visibility and Selection, and publishes an immutable snapshot the
// way RetainedPreviewStore already publishes preview scenes and watch traces.
// Any thread may publish; the render thread retains a shared_ptr and never
// takes the store lock.
//
// ⚠️ A BATCH IS ATOMIC. Everything between BeginBatch and EndBatch is STAGED,
// and swapped in one step at EndBatch. A batch cut short by a worker death is
// discarded whole, never half-applied, which mirrors SceneCmdQueue's
// BeginBatch/EndBatch contract deliberately: half a batch on screen is a
// picture of a building that never existed, and nothing in it says so.
//
// ⚠️ EPOCH BEFORE REVISION, ALWAYS. `revision` is monotonic within one worker
// generation; `epoch` changes on every worker restart. A message from a stale
// epoch is dropped without complaint -- after a kill and restart the host holds
// preview from a process that no longer exists, and DropAll plus a fresh epoch
// is the only correct answer.
//
// ⚠️ NO GPU, NO DevKit, NO Win32, NO ACAPI IN HERE. This is CPU-side cache
// semantics, which is exactly the half that can be proved offline; the GPU half
// is ArchViz/GhPreviewLayer and the handover is SceneCmdQueue. Keeping this file
// free of all four is what keeps the delta protocol under an offline gate.

#include "Grasshopper/GhPreviewProtocol.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace evp::preview {

using evp::grasshopper::protocol::PreviewChange;
using evp::grasshopper::protocol::PreviewKind;
using evp::grasshopper::protocol::PreviewPayloadDescriptor;
using evp::grasshopper::protocol::PreviewPrimitiveHeader;
using evp::grasshopper::protocol::PreviewPrimitiveMessage;

// One primitive as the host holds it: the metadata that identifies it, and CPU
// buffers ready to be uploaded. Immutable once published.
//
// The arrays are already COPIED OUT of the worker's shared-memory segment by
// the time one of these exists. Nothing here ever points into another process.
struct GhPreviewPrimitive {
    uint64_t id = 0;
    PreviewKind kind = PreviewKind::Polyline3D;
    uint8_t flags = 0;
    uint32_t itemIndex = 0;
    uint8_t componentGuid[16] = {};
    uint8_t parameterGuid[16] = {};
    uint32_t branchHash = 0;
    uint64_t contentHash = 0;
    uint32_t revision = 0;
    bool closed = false;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    std::string text;

    uint32_t PointCount () const
    {
        return (uint32_t) (positions.size () / 3);
    }
    bool Visible () const
    {
        return (flags & evp::grasshopper::protocol::PreviewFlagVisible) != 0;
    }
    bool Selected () const
    {
        return (flags & evp::grasshopper::protocol::PreviewFlagSelected) != 0;
    }
};

// What the render thread reads. Entries are shared_ptr so that swapping a
// snapshot costs pointer copies rather than re-copying every mesh: a batch that
// changes one component's output must not cost the whole preview again.
struct GhPreviewSnapshot {
    uint64_t generation = 0;
    uint32_t epoch = 0;
    uint32_t revision = 0;
    std::vector<std::shared_ptr<const GhPreviewPrimitive>> primitives;
};

// Why a message did not change anything. Refused and Ignored are different on
// purpose: Refused is the worker's fault and must be reported and resynced,
// Ignored is a stale epoch and is normal after a restart.
enum class GhPreviewApply {
    Applied,
    Ignored, // stale epoch, or a batch that was already abandoned
    Refused, // malformed, over a ceiling, or out of sequence
};

struct GhPreviewEndResult {
    GhPreviewApply apply = GhPreviewApply::Refused;
    // True when the batch's ids do not sum to what PreviewEndBatch declared.
    // The host must ask for a full resync rather than draw a cache it cannot
    // trust -- cheap insurance against a dropped or reordered message becoming
    // a permanently wrong viewport.
    bool resyncRequired = false;
    std::string reason;
};

class GhPreviewCache {
  public:
    // Adopts a new worker generation and forgets everything. The correct answer
    // to a worker restart, a closed definition, and a failed send on the
    // worker's side (its mirror has already advanced, so retrying against it
    // would leave the two sides disagreeing).
    void DropAll (uint32_t epoch, const std::string& reason);

    // Opens a staged batch. Refuses a second one: two open batches would make
    // "atomic" meaningless, and the only way it happens is a worker bug.
    GhPreviewApply BeginBatch (const evp::grasshopper::protocol::PreviewBeginBatchPayload& begin, std::string& reason);

    // Added and Changed are one call: the cache does not care which the worker
    // called it, only that this id now holds this content. The DISTINCTION
    // matters to the checksum, which is why the change kind is passed in.
    GhPreviewApply Apply (const PreviewPrimitiveMessage& message, PreviewChange change, std::string& reason);

    GhPreviewApply Remove (const evp::grasshopper::protocol::PreviewIdRunPayload& run, std::string& reason);

    // Visibility and Selection. Flags only, and geometry is never touched:
    // toggling a component's preview off must cost a byte, not a
    // retransmission, and a selection change over 500 primitives must move no
    // vertices at all.
    GhPreviewApply SetFlags (const evp::grasshopper::protocol::PreviewIdRunPayload& run, std::string& reason);

    // Swaps the staged batch in, in one step, and publishes a new snapshot.
    GhPreviewEndResult EndBatch (const evp::grasshopper::protocol::PreviewEndBatchPayload& end);

    // Throws away a staged batch without publishing it. What a worker death
    // mid-batch means.
    void AbandonBatch (const std::string& reason);

    std::shared_ptr<const GhPreviewSnapshot> SnapshotCopy () const;

    uint32_t Epoch () const;
    bool BatchOpen () const;
    // How many primitives the live cache holds. Staged changes are not counted
    // until they are swapped in.
    std::size_t Count () const;

  private:
    using Map = std::unordered_map<uint64_t, std::shared_ptr<const GhPreviewPrimitive>>;

    // Called with `mutex` held.
    void PublishLocked ();

    mutable std::mutex mutex;
    uint64_t generation = 0;
    uint32_t epoch = 0;
    uint32_t revision = 0;
    Map live;
    std::shared_ptr<const GhPreviewSnapshot> published;

    bool batchOpen = false;
    uint32_t batchRevision = 0;
    uint32_t batchExpected = 0;
    uint32_t batchEntries = 0;
    uint64_t batchChecksum = 0;
    Map staged;
};

} // namespace evp::preview

#endif
