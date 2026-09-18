#include "ArchViz/SceneCmdQueue.hpp"

#include "ArchViz/Dxgi/HostOccluders.hpp"

#include <algorithm>

namespace geomsrv {
namespace archviz {

size_t ElementUpload::Bytes () const
{
    return guid.capacity () + vertices.capacity () * sizeof (float) + normals.capacity () * sizeof (float) +
           indices.capacity () * sizeof (uint32_t) + ranges.capacity () * sizeof (MaterialRange) +
           wireEdges.capacity () * sizeof (uint32_t);
}

size_t PointLayerUpload::Bytes () const
{
    return layerId.capacity () + sourceId.capacity () + sourcePath.capacity ();
}

size_t PointNodeUpload::Bytes () const
{
    return layerId.capacity () + vertices.capacity () * sizeof (PointCloudVertex);
}

size_t StorySliceUpload::Bytes () const
{
    return outline.capacity () * sizeof (StorySliceVertex) + fill.capacity () * sizeof (StorySliceFillVertex);
}

SceneCmdQueue& SceneCmdQueue::Get ()
{
    static SceneCmdQueue instance;
    return instance;
}

namespace {

// ⚠️ THE OPACITY DECISION LIVES HERE, WHERE THE MODEL IS, AND NOWHERE
// ELSE. This is the one place that holds both an element's material-grouped
// ranges and the material table those ranges index, so it is the only place that
// can say "this triangle belongs to a wall and that one belongs to glass". The
// D3D11 side is handed triangles that are ALREADY classified and never sees a
// material -- which is the separation runs forty-eight to fifty-one paid for:
//
//     the GPU hook answers   WHERE is the camera, WHEN do we compose
//     the model answers      WHAT is opaque
//
// ⚠️ AND IT IS `alpha`, NOT A NAME AND NOT A D3D BLEND STATE.
// `SurfaceMaterial::alpha` is Archicad's own transparency, flipped to opacity by
// `MaterialTable`, and `kOpaqueAlpha` is the threshold it already uses.
// `SurfaceClassifier` documents at length why names may never decide a surface's
// type; run forty-nine documents why blend state may not either -- all 2914
// scene draws report `BlendEnable = TRUE`.
const MaterialTable* g_hostMaterials = nullptr;

void FeedOpaqueOccluders (const ElementUpload& upload)
{
    if (g_hostMaterials == nullptr || upload.vertices.empty () || upload.ranges.empty ())
        return;

    // ⚠️ THE VERTEX BLOCK GOES IN ONCE, NOT ONCE PER RANGE. An element is
    // material-grouped into several ranges and the first version of this handed
    // the whole vertex array to each of them, copying every wall as many times
    // as it had materials.
    const uint32_t base = dxgi::hostocclusion::AddVertices (upload.vertices.data (), uint32_t (upload.VertexCount ()));
    if (base == dxgi::hostocclusion::kNoBase)
        return;

    // The ranges are already contiguous per material, so an opaque material is
    // one contiguous run of indices and needs no per-triangle work.
    for (const MaterialRange& range : upload.ranges) {
        if (range.indexCount == 0)
            continue;
        if (size_t (range.firstIndex) + range.indexCount > upload.indices.size ())
            continue;
        const SurfaceMaterial& material = g_hostMaterials->Lookup (range.material);
        if (material.alpha < kOpaqueAlpha) {
            // Glass, a build plane, a helper: it may not hide us -- and it is
            // still part of the building, so it keeps its feature edges. See
            // `AddTransparentIndices`. Counted as well, so "no opaque geometry"
            // can still be told apart from "no geometry at all".
            dxgi::hostocclusion::NoteTransparent (range.indexCount);
            dxgi::hostocclusion::AddTransparentIndices (base, upload.indices.data () + range.firstIndex,
                                                        range.indexCount);
            continue;
        }
        dxgi::hostocclusion::AddOpaqueIndices (base, upload.indices.data () + range.firstIndex, range.indexCount);
    }
}

} // namespace

void SceneCmdQueue::PushBeginBatch (bool full)
{
    // ⚠️ THE OCCLUDER BATCH FOLLOWS THE SCENE BATCH EXACTLY, so a
    // full rebuild replaces the occluder rather than adding a second copy of the
    // building to it.
    dxgi::hostocclusion::BeginBatch (full);
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::BeginBatch;
    cmd.full = full;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushUpsert (std::unique_ptr<ElementUpload> upload)
{
    if (upload == nullptr)
        return; // nothing to hand over; a null node would be a consumer crash

    // ⚠️ BEFORE THE MOVE, BECAUSE AFTER IT THERE IS NOTHING TO READ.
    FeedOpaqueOccluders (*upload);

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += upload->Bytes ();
    SceneCmd cmd;
    cmd.type = SceneCmdType::UpsertElement;
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
    // Publish the occluder snapshot in one exchange. Until this runs the render
    // thread keeps the previous building, so a half-extracted model never
    // occludes anything.
    dxgi::hostocclusion::EndBatch ();
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::EndBatch;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushMaterials (std::unique_ptr<MaterialTable> materials)
{
    if (materials == nullptr)
        return; // same rule as PushUpsert: a null node would be a consumer crash

    // ⚠️ THE TABLE IS RETAINED FOR THE CLASSIFIER, AND IT ARRIVES
    // BEFORE THE ELEMENTS DO. `ExtractionThread` pushes materials first; an
    // element that somehow arrived earlier is fed nothing rather than being
    // guessed at, which is why `FeedOpaqueOccluders` returns on a null table.
    static std::unique_ptr<MaterialTable> retained;
    retained = std::make_unique<MaterialTable> (*materials);
    g_hostMaterials = retained.get ();

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += materials->Bytes ();
    SceneCmd cmd;
    cmd.type = SceneCmdType::SetMaterials;
    cmd.materials = std::move (materials);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushEnvironment (const EnvironmentUpload& environment)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::SetEnvironment;
    cmd.environment = environment;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushSelection (std::vector<std::string> guids)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::SetSelection;
    cmd.selection = std::move (guids);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushStorySlices (std::unique_ptr<StorySliceUpload> slices)
{
    if (slices == nullptr)
        return; // same rule as PushUpsert: a null node would be a consumer crash

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += slices->Bytes ();
    SceneCmd cmd;
    cmd.type = SceneCmdType::SetStorySlices;
    cmd.storySlices = std::move (slices);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushBeginPointLayer (std::unique_ptr<PointLayerUpload> layer)
{
    if (layer == nullptr)
        return;

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += layer->Bytes ();
    SceneCmd cmd;
    cmd.type = SceneCmdType::BeginPointLayer;
    cmd.pointLayer = std::move (layer);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushClearPointLayer (const std::string& layerId)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::ClearPointLayer;
    cmd.pointLayerId = layerId;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushUpsertPointNode (std::unique_ptr<PointNodeUpload> node)
{
    if (node == nullptr)
        return;

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += node->Bytes ();
    SceneCmd cmd;
    cmd.type = SceneCmdType::UpsertPointNode;
    cmd.pointNode = std::move (node);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushEndPointLayer (const std::string& layerId)
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::EndPointLayer;
    cmd.pointLayerId = layerId;
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushSunStudyAtlas (std::unique_ptr<SunStudyAtlasUpload> study)
{
    if (study == nullptr)
        return; // a null node would be a consumer crash; see PushUpsert

    std::lock_guard<std::mutex> lock (mutex_);
    pendingBytes_ += study->Bytes ();
    SceneCmd cmd;
    cmd.type = SceneCmdType::SetSunStudyAtlas;
    cmd.sunStudy = std::move (study);
    queue_.push_back (std::move (cmd));
}

void SceneCmdQueue::PushClearSunStudy ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    SceneCmd cmd;
    cmd.type = SceneCmdType::ClearSunStudy;
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
        if (queue_[i].pointLayer != nullptr)
            bytes += queue_[i].pointLayer->Bytes ();
        if (queue_[i].pointNode != nullptr)
            bytes += queue_[i].pointNode->Bytes ();
        if (queue_[i].storySlices != nullptr)
            bytes += queue_[i].storySlices->Bytes ();
        if (queue_[i].sunStudy != nullptr)
            bytes += queue_[i].sunStudy->Bytes ();
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

} // namespace archviz
} // namespace geomsrv
