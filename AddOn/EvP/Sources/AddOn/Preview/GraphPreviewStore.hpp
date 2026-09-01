#ifndef EVP_PREVIEW_GRAPHPREVIEWSTORE_HPP
#define EVP_PREVIEW_GRAPHPREVIEWSTORE_HPP

// What the NODE GRAPH is previewing.
//
// ⚠️ THIS IS A SECOND STORE ON PURPOSE, AND MUST NOT BE FOLDED INTO
// GhPreviewCache. That cache is a MIRROR of another process: its whole shape --
// epochs, revisions, one open batch at a time, a checksum, a resync request --
// exists because the producer is a Grasshopper worker that can die mid-batch and
// whose deltas have to be reconciled across a pipe. Every one of those rules
// assumes ONE producer. The node graph runs in this process, recomputes the
// whole answer on every evaluation, and cannot half-send anything; pushed into
// the same cache it would fight the worker for the open batch and reset its
// epoch, and the first symptom would be a Grasshopper preview that silently
// stops updating while a graph is open.
//
// So the two producers keep their own stores and MERGE at the point of drawing,
// where merging is a pointer copy: the drawables builder, the layer, the shaders
// and the limits are all shared, and nothing here duplicates any of them.
//
// ⚠️ A GRAPH PUBLISHES ITS WHOLE PREVIEW AT ONCE. There is no per-node delta and
// there should not be: a node deleted, disabled, failed, or wired to something
// that now produces nothing must have its geometry DISAPPEAR, and every one of
// those is an absence rather than an event. Replacing the graph's whole set makes
// absence the default; a delta protocol would need a withdrawal message for each
// of them, and the one nobody wrote is a preview of geometry the graph no longer
// produces -- which is worse than no preview, because it looks authoritative.
//
// Pure: no GPU, no DevKit, no ACAPI, no Win32. Same rule as GhPreviewCache beside
// it, and for the same reason -- this is the half an offline test can pin.

#include "Preview/GhPreviewCache.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace evp::preview {

// A primitive id that no Grasshopper primitive can ever equal.
//
// ⚠️ THE TOP BIT IS THE PRODUCER, AND IT IS NOT DECORATION. The drawables builder
// orders by id, and the two stores are merged before it runs; ids that could
// collide would make one producer's primitive shadow the other's in a way that
// depends on a hash seed. The worker allocates ids from a counter, so the top bit
// is free, and setting it here partitions the space by construction rather than
// by hoping.
constexpr uint64_t kGraphPreviewIdBit = 1ull << 63;

// Stable across runs: the same node previewing the same item keeps the same id,
// so an unchanged preview produces an identical snapshot and the renderer's
// generation compare does its job.
uint64_t GraphPreviewPrimitiveId (const std::string& graphId, const std::string& nodeId, uint32_t item);

// The two producers, as one thing to draw.
//
// ⚠️ MERGING IS A POINTER COPY, WHICH IS WHY THE STORES CAN STAY SEPARATE. A
// snapshot holds shared_ptr primitives, so combining them costs one vector of
// pointers and never touches a vertex.
struct MergedPreview {
    // Null when neither producer has ever published.
    std::shared_ptr<const GhPreviewSnapshot> snapshot;

    // ⚠️ ONE NUMBER THAT CHANGES WHEN EITHER HALF DOES, and NOT their sum: the
    // renderer rebuilds its buffers only when this changes, so two generations
    // moving in opposite directions by the same amount would freeze the viewport
    // on old geometry with nothing in the log to say so. Zero means nothing has
    // ever been published, which is the renderer's "do not build a layer at all".
    uint64_t generation = 0;
};

MergedPreview MergePreviewSnapshots (const std::shared_ptr<const GhPreviewSnapshot>& grasshopper,
                                     const std::shared_ptr<const GhPreviewSnapshot>& graph);

class GraphPreviewStore {
  public:
    // Process-wide, reached the way GhPreviewCache is and for the same reason:
    // the producer (the graph runtime) and the consumer (the Diligent scene)
    // have no object in common, and threading a reference from one to the other
    // would put the graph runtime in the renderer's headers.
    //
    // ⚠️ AN ACCESSOR, NOT A SINGLETON CONTRACT. The tests build their own.
    static GraphPreviewStore& Get ();

    // Everything this graph shows, replacing whatever it showed before. An empty
    // vector is a meaningful call: it is how a graph with no enabled preview node
    // says so.
    void PublishGraph (const std::string& graphId, std::vector<std::shared_ptr<const GhPreviewPrimitive>> primitives);

    // The graph is gone -- closed, or its document dropped. Distinct from
    // publishing nothing only in that it stops costing a map entry.
    void ClearGraph (const std::string& graphId);

    // Every graph. What a project close means.
    void ClearAll ();

    // Null until something has been published. The renderer holds this across
    // frames and never takes the lock.
    std::shared_ptr<const GhPreviewSnapshot> SnapshotCopy () const;

    uint64_t Generation () const;
    std::size_t Count () const;
    std::size_t GraphCount () const;

  private:
    // Called with `mutex_` held.
    void PublishLocked ();

    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    // Ordered, so the published snapshot's order depends on the graph ids and
    // not on a hash seed. The builder sorts by id anyway; this makes the
    // snapshot itself reproducible, which is what a test can assert on.
    std::map<std::string, std::vector<std::shared_ptr<const GhPreviewPrimitive>>> graphs_;
    std::shared_ptr<const GhPreviewSnapshot> published_;
};

} // namespace evp::preview

#endif
