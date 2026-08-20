#include "ArchViz/SceneCmdQueue.hpp"

#include <algorithm>

namespace geomsrv {
namespace archviz {

size_t ElementUpload::Bytes () const
{
    return guid.capacity ()
         + vertices.capacity () * sizeof (float)
         + normals.capacity ()  * sizeof (float)
         + indices.capacity ()  * sizeof (uint32_t)
         + ranges.capacity ()   * sizeof (MaterialRange);
}

SceneCmdQueue& SceneCmdQueue::Get ()
{
    static SceneCmdQueue instance;
    return instance;
}

void SceneCmdQueue::PushBeginBatch (bool full)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::BeginBatch;
    cmd.full = full;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushUpsert (std::unique_ptr<ElementUpload> upload)
{
    if (upload == nullptr)
        return;   // nothing to hand over; a null node would be a consumer crash

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += upload->Bytes ();
    SceneCmd cmd;
    cmd.type   = SceneCmdType::UpsertElement;
    cmd.upload = std::move (upload);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushRemove (const std::string& guid)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::RemoveElement;
    cmd.guid = guid;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushEndBatch ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::EndBatch;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushMaterials (std::unique_ptr<MaterialTable> materials)
{
    if (materials == nullptr)
        return;   // same rule as PushUpsert: a null node would be a consumer crash

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += materials->Bytes ();
    SceneCmd cmd;
    cmd.type      = SceneCmdType::SetMaterials;
    cmd.materials = std::move (materials);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushEnvironment (const EnvironmentUpload& environment)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type        = SceneCmdType::SetEnvironment;
    cmd.environment = environment;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushSelection (std::vector<std::string> guids)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type      = SceneCmdType::SetSelection;
    cmd.selection = std::move (guids);
    queue_.push_back (std::move (cmd));
}

std::vector<SceneCmd> SceneCmdQueue::Take (size_t max)
{
    std::vector<SceneCmd> out;
    if (max == 0)
        return out;

    std::lock_guard<std::mutex> lock (mutex_);
    const size_t take = std::min (max, queue_.size ());
    out.reserve (take);

    for (size_t i = 0; i < take; ++i) {
        // The accounting has to drop as the payload leaves, not when the
        // consumer eventually frees it — PendingBytes answers "how much is
        // waiting to be uploaded", and a producer throttling on it would
        // otherwise never see the queue drain.
        size_t bytes = 0;
        if (queue_[i].upload != nullptr)
            bytes += queue_[i].upload->Bytes ();
        if (queue_[i].materials != nullptr)
            bytes += queue_[i].materials->Bytes ();
        pendingBytes_ = (pendingBytes_ > bytes) ? (pendingBytes_ - bytes) : 0;
        out.push_back (std::move (queue_[i]));
    }
    // ⚠️ ERASE THE FRONT, KEEP THE ORDER. The commands are a SEQUENCE:
    // BeginBatch(full) then upserts then EndBatch. Swap-and-pop here would be
    // cheaper and would reorder a rebuild into nonsense — an EndBatch arriving
    // before its upserts makes a full batch drop the elements it was about to
    // receive.
    queue_.erase (queue_.begin (), queue_.begin () + ptrdiff_t (take));
    return out;
}

void SceneCmdQueue::Clear ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    queue_.clear ();
    pendingBytes_ = 0;
}

size_t SceneCmdQueue::PendingCount () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return queue_.size ();
}

size_t SceneCmdQueue::PendingBytes () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return pendingBytes_;
}

}   // namespace archviz
}   // namespace geomsrv
