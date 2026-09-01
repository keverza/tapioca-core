#include "Preview/GraphPreviewStore.hpp"

#include <utility>

namespace evp::preview {

uint64_t GraphPreviewPrimitiveId (const std::string& graphId, const std::string& nodeId, uint32_t item)
{
    // FNV-1a over the two identities and the item index. A hash rather than a
    // counter because the id has to be the SAME on the next run for an unchanged
    // preview to produce an unchanged snapshot, and a counter would depend on
    // the order the nodes happened to be walked in.
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash] (const std::string& text) {
        for (const char character : text) {
            hash ^= static_cast<uint64_t> (static_cast<unsigned char> (character));
            hash *= 1099511628211ull;
        }
        // A separator, so ("ab","c") and ("a","bc") are different ids.
        hash ^= 0xFFull;
        hash *= 1099511628211ull;
    };
    mix (graphId);
    mix (nodeId);
    for (int byte = 0; byte < 4; ++byte) {
        hash ^= static_cast<uint64_t> ((item >> (byte * 8)) & 0xFFu);
        hash *= 1099511628211ull;
    }
    return hash | kGraphPreviewIdBit;
}

MergedPreview MergePreviewSnapshots (const std::shared_ptr<const GhPreviewSnapshot>& grasshopper,
                                     const std::shared_ptr<const GhPreviewSnapshot>& graph)
{
    const uint64_t ghGeneration = grasshopper != nullptr ? grasshopper->generation : 0;
    const uint64_t graphGeneration = graph != nullptr ? graph->generation : 0;

    MergedPreview merged;
    if (ghGeneration == 0 && graphGeneration == 0)
        return merged;
    merged.generation = (ghGeneration * 1000003ull) ^ (graphGeneration + 1ull);

    const bool ghEmpty = grasshopper == nullptr || grasshopper->primitives.empty ();
    const bool graphEmpty = graph == nullptr || graph->primitives.empty ();

    // Either side alone is handed straight through: the common case is one
    // producer, and copying its vector to prove a point would be the largest
    // allocation on this path.
    if (graphEmpty) {
        merged.snapshot = grasshopper;
        return merged;
    }
    if (ghEmpty) {
        merged.snapshot = graph;
        return merged;
    }

    auto combined = std::make_shared<GhPreviewSnapshot> (*grasshopper);
    combined->primitives.insert (combined->primitives.end (), graph->primitives.begin (), graph->primitives.end ());
    merged.snapshot = std::move (combined);
    return merged;
}

GraphPreviewStore& GraphPreviewStore::Get ()
{
    static GraphPreviewStore instance;
    return instance;
}

void GraphPreviewStore::PublishGraph (const std::string& graphId,
                                      std::vector<std::shared_ptr<const GhPreviewPrimitive>> primitives)
{
    std::lock_guard lock (mutex_);
    if (primitives.empty ()) {
        // Publishing nothing and clearing are the same state, so they are the
        // same entry: an empty vector left in the map would make GraphCount lie.
        // Nothing to republish when the graph already showed nothing.
        if (graphs_.erase (graphId) == 0)
            return;
    }
    else {
        graphs_[graphId] = std::move (primitives);
    }
    PublishLocked ();
}

void GraphPreviewStore::ClearGraph (const std::string& graphId)
{
    std::lock_guard lock (mutex_);
    if (graphs_.erase (graphId) == 0)
        return;
    PublishLocked ();
}

void GraphPreviewStore::ClearAll ()
{
    std::lock_guard lock (mutex_);
    if (graphs_.empty ())
        return;
    graphs_.clear ();
    PublishLocked ();
}

void GraphPreviewStore::PublishLocked ()
{
    auto snapshot = std::make_shared<GhPreviewSnapshot> ();
    snapshot->generation = ++generation_;
    for (const auto& entry : graphs_) {
        for (const auto& primitive : entry.second)
            snapshot->primitives.push_back (primitive);
    }
    published_ = std::move (snapshot);
}

std::shared_ptr<const GhPreviewSnapshot> GraphPreviewStore::SnapshotCopy () const
{
    std::lock_guard lock (mutex_);
    return published_;
}

uint64_t GraphPreviewStore::Generation () const
{
    std::lock_guard lock (mutex_);
    return generation_;
}

std::size_t GraphPreviewStore::Count () const
{
    std::lock_guard lock (mutex_);
    std::size_t count = 0;
    for (const auto& entry : graphs_)
        count += entry.second.size ();
    return count;
}

std::size_t GraphPreviewStore::GraphCount () const
{
    std::lock_guard lock (mutex_);
    return graphs_.size ();
}

} // namespace evp::preview
