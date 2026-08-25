#include "Preview/RetainedPreviewStore.hpp"

#include <utility>

namespace evp::preview {

const WatchNode& SelectedWatchFrameSnapshot::Node () const
{
    return snapshot->trace.nodes[nodeIndex];
}

const WatchFrame& SelectedWatchFrameSnapshot::Frame () const
{
    return Node ().frames[frameIndex];
}

RetainedPreviewStore& RetainedPreviewStore::Get ()
{
    static RetainedPreviewStore store;
    return store;
}

uint64_t RetainedPreviewStore::PublishPreviewScene (PreviewScene scene)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto snapshot = std::make_shared<PreviewSceneSnapshot> ();
    snapshot->generation = ++generation;
    snapshot->scene = std::move (scene);
    previewScene = std::move (snapshot);
    return generation;
}

uint64_t RetainedPreviewStore::PublishWatchTrace (WatchTrace trace)
{
    std::lock_guard<std::mutex> lock (mutex);
    auto snapshot = std::make_shared<WatchTraceSnapshot> ();
    snapshot->generation = ++generation;
    snapshot->trace = std::move (trace);
    watchTrace = std::move (snapshot);
    selectedWatchNode.reset ();
    selectedWatchFrame.reset ();
    for (std::size_t nodeIndex = 0; nodeIndex < watchTrace->trace.nodes.size (); ++nodeIndex) {
        if (!watchTrace->trace.nodes[nodeIndex].frames.empty ()) {
            selectedWatchNode = nodeIndex;
            selectedWatchFrame = 0;
            break;
        }
    }
    return generation;
}

std::shared_ptr<const PreviewSceneSnapshot> RetainedPreviewStore::PreviewSceneSnapshotCopy () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return previewScene;
}

std::shared_ptr<const WatchTraceSnapshot> RetainedPreviewStore::WatchTraceSnapshotCopy () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return watchTrace;
}

std::optional<SelectedWatchFrameSnapshot> RetainedPreviewStore::SelectedWatchFrameSnapshotCopy () const
{
    std::lock_guard<std::mutex> lock (mutex);
    if (watchTrace == nullptr || !selectedWatchNode.has_value () || !selectedWatchFrame.has_value ())
        return std::nullopt;
    return SelectedWatchFrameSnapshot { watchTrace, *selectedWatchNode, *selectedWatchFrame };
}

bool RetainedPreviewStore::SelectWatchFrame (std::size_t nodeIndex, std::size_t frameIndex)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (watchTrace == nullptr || nodeIndex >= watchTrace->trace.nodes.size () ||
        frameIndex >= watchTrace->trace.nodes[nodeIndex].frames.size ())
        return false;
    selectedWatchNode = nodeIndex;
    selectedWatchFrame = frameIndex;
    return true;
}

void RetainedPreviewStore::ClearWatchSelection ()
{
    std::lock_guard<std::mutex> lock (mutex);
    selectedWatchNode.reset ();
    selectedWatchFrame.reset ();
}

} // namespace evp::preview
