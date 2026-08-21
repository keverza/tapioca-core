// The scene's ELEMENT MAP: mesh buffer creation, the SceneCmdQueue drain, the
// scene bounds and the stats derived from all of it. The pipeline states live in
// DiligentScene.cpp and the passes in DiligentSceneDraw.cpp; DiligentSceneImpl.hpp
// says why the class is spread over three files.

#include "ArchViz/AutoExposure.hpp"
#include "ArchViz/DiligentSceneImpl.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

bool CreateMeshBuffers (Diligent::IRenderDevice* device, const char* name, Entry& entry,
                        const void* vertices, size_t vertexBytes,
                        const void* indices, size_t indexBytes, std::string& error)
{
    Diligent::BufferDesc vbd;
    vbd.Name = name;
    vbd.Size = vertexBytes;
    vbd.BindFlags = Diligent::BIND_VERTEX_BUFFER;
    // IMMUTABLE: an element is replaced wholesale, never edited in place. A
    // changed element is a new buffer, exactly as it was a new bgfx handle.
    vbd.Usage = Diligent::USAGE_IMMUTABLE;
    const Diligent::BufferData vertexData {vertices, vertexBytes};
    // ⚠️ RELEASE BEFORE OVERWRITING. An upsert of an already-cached element is
    // the ORDINARY case during live sync; dropping the old buffers on the floor
    // leaks GPU memory once per edit, invisible until a long session runs the
    // driver out.
    entry.vertexBuffer.Release ();
    entry.indexBuffer.Release ();
    device->CreateBuffer (vbd, &vertexData, &entry.vertexBuffer);

    Diligent::BufferDesc ibd;
    ibd.Name = name;
    ibd.Size = indexBytes;
    ibd.BindFlags = Diligent::BIND_INDEX_BUFFER;
    ibd.Usage = Diligent::USAGE_IMMUTABLE;
    const Diligent::BufferData indexData {indices, indexBytes};
    device->CreateBuffer (ibd, &indexData, &entry.indexBuffer);

    if (entry.vertexBuffer == nullptr || entry.indexBuffer == nullptr) {
        error = std::string ("Diligent CreateBuffer failed for '") + name + "'";
        entry.vertexBuffer.Release ();
        entry.indexBuffer.Release ();
        return false;
    }
    entry.gpuBytes = vertexBytes + indexBytes;
    return true;
}

bool DiligentScene::AddStaticMesh (Diligent::IRenderDevice* device, const char* name,
                                   const ArchVizVertex* vertices, size_t vertexCount,
                                   const uint16_t* indices, size_t indexCount,
                                   std::string& error)
{
    if (device == nullptr || vertices == nullptr || indices == nullptr ||
        vertexCount == 0 || indexCount == 0) {
        error = "DiligentScene::AddStaticMesh got an empty or null mesh";
        return false;
    }

    Entry entry;
    entry.indices32 = false;
    if (!CreateMeshBuffers (device, name, entry, vertices, vertexCount * sizeof (ArchVizVertex),
                            indices, indexCount * sizeof (uint16_t), error))
        return false;
    entry.vertexCount = Diligent::Uint32 (vertexCount);
    entry.indexCount = Diligent::Uint32 (indexCount);
    // One range covering everything, material -1 so the table is never consulted
    // and the miss counter is not polluted by geometry that has no material.
    entry.ranges.push_back (MaterialRange {-1, 0, uint32_t (indexCount)});
    impl_->staticMeshes.push_back (std::move (entry));
    return true;
}

void DiligentScene::ClearStaticMeshes () { impl_->staticMeshes.clear (); }

bool DiligentScene::AddOverlayMesh (Diligent::IRenderDevice* device, const char* name,
                                    const ArchVizVertex* vertices, size_t vertexCount,
                                    const uint16_t* indices, size_t indexCount,
                                    std::string& error)
{
    if (device == nullptr || vertices == nullptr || indices == nullptr ||
        vertexCount == 0 || indexCount == 0) {
        error = "DiligentScene::AddOverlayMesh got an empty or null mesh";
        return false;
    }

    Entry entry;
    entry.indices32 = false;
    if (!CreateMeshBuffers (device, name, entry, vertices, vertexCount * sizeof (ArchVizVertex),
                            indices, indexCount * sizeof (uint16_t), error))
        return false;
    entry.vertexCount = Diligent::Uint32 (vertexCount);
    entry.indexCount = Diligent::Uint32 (indexCount);
    entry.ranges.push_back (MaterialRange {-1, 0, uint32_t (indexCount)});
    impl_->overlayMeshes.push_back (std::move (entry));
    return true;
}

size_t DiligentScene::Consume (Diligent::IRenderDevice* device, size_t maxCommands)
{
    if (device == nullptr || !impl_->ready)
        return 0;

    // ⚠️ Take() HANDS OVER OWNERSHIP OF A BATCH. The nodes hold unique_ptrs the
    // producer has already let go of, so anything not consumed here is freed
    // here -- there is no putting one back.
    std::vector<SceneCmd> batch = SceneCmdQueue::Get ().Take (maxCommands);
    size_t applied = 0;
    const size_t elementsBefore = impl_->elements.size ();
    for (SceneCmd& cmd : batch) {
        ++applied;

        switch (cmd.type) {
            case SceneCmdType::BeginBatch:
                impl_->inFullBatch = cmd.full;
                if (cmd.full) {
                    for (Entry& e : impl_->elements)
                        e.seenThisBatch = false;
                }
                break;

            case SceneCmdType::SetMaterials:
                // ⚠️ WHOLE-TABLE REPLACE, and it must land BEFORE its batch's
                // upserts. The pool is renumbered on every rebuild.
                if (cmd.materials != nullptr)
                    impl_->materials = std::move (*cmd.materials);
                break;

            case SceneCmdType::SetEnvironment: {
                const EnvironmentUpload& env = cmd.environment;
                // ⚠️ NORMALISED HERE, not by the producer: a sun below the
                // horizon can arrive as a zero-length vector, and normalize(0)
                // in a shader is a NaN that renders as black geometry.
                const float length = std::sqrt (env.sunX * env.sunX + env.sunY * env.sunY +
                                                env.sunZ * env.sunZ);
                if (length > 1e-6f) {
                    impl_->sun[0] = env.sunX / length;
                    impl_->sun[1] = env.sunY / length;
                    impl_->sun[2] = env.sunZ / length;
                } else {
                    impl_->sun[0] = 0.0f;
                    impl_->sun[1] = 0.0f;
                    impl_->sun[2] = 1.0f;
                }
                impl_->ambient = env.ambient;
                impl_->sunBelowHorizon = env.sunBelowHorizon;
                impl_->northDegrees = env.northDegrees;
                impl_->latitudeDegrees = env.latitudeDegrees;
                impl_->longitudeDegrees = env.longitudeDegrees;
                impl_->siteAltitudeMetres = env.altitudeMetres;
                impl_->year = env.year;
                impl_->month = env.month;
                impl_->day = env.day;
                impl_->hour = env.hour;
                impl_->minute = env.minute;
                impl_->summerTime = env.summerTime;
                impl_->haveComputedSun = env.haveComputedSun;
                impl_->computedAzimuthDegrees = env.computedAzimuthDegrees;
                impl_->computedAltitudeDegrees = env.computedAltitudeDegrees;
                impl_->sunApplied = true;
                break;
            }

            case SceneCmdType::UpsertElement: {
                if (cmd.upload == nullptr)
                    break;
                const ElementUpload& upload = *cmd.upload;
                const uint32_t verts = uint32_t (upload.VertexCount ());
                if (verts == 0 || upload.indices.empty ())
                    break;

                std::vector<ArchVizVertex> interleaved (verts);
                for (uint32_t i = 0; i < verts; ++i) {
                    interleaved[i].x = upload.vertices[i * 3 + 0];
                    interleaved[i].y = upload.vertices[i * 3 + 1];
                    interleaved[i].z = upload.vertices[i * 3 + 2];
                    // A missing normal is a flat surface, not a crash. The
                    // extractor always supplies them, but a short array must not
                    // read past the end.
                    const bool haveN = (i * 3 + 2) < upload.normals.size ();
                    interleaved[i].nx = haveN ? upload.normals[i * 3 + 0] : 0.0f;
                    interleaved[i].ny = haveN ? upload.normals[i * 3 + 1] : 0.0f;
                    interleaved[i].nz = haveN ? upload.normals[i * 3 + 2] : 1.0f;
                    // WHITE. The surface colour is a per-range CONSTANT, not a
                    // vertex attribute: VertexWeld merges corners that share a
                    // position and a normal, and two coplanar polygons with
                    // different surfaces do exactly that -- baking the colour
                    // per vertex bleeds a gradient across the seam.
                    interleaved[i].abgr = 0xffffffff;
                }

                Entry* existing = impl_->Find (upload.guid);
                Entry fresh;
                Entry& e = existing != nullptr ? *existing : fresh;

                std::string error;
                if (!CreateMeshBuffers (device, "ArchViz element", e,
                                        interleaved.data (), interleaved.size () * sizeof (ArchVizVertex),
                                        upload.indices.data (), upload.indices.size () * sizeof (uint32_t),
                                        error))
                    break;

                e.guid = upload.guid;
                // ⚠️ ASSIGNED ONCE, AND ONLY ONCE. An upsert of an element that
                // is already cached is the ordinary case during live sync;
                // renumbering it there would invalidate a pick already in flight
                // and, worse, break the selection the user is looking at every
                // time anything in the project changed.
                if (e.pickId == kNoPickId)
                    e.pickId = impl_->nextPickId++;
                e.selected = impl_->IsSelectedGuid (e.guid);
                // ⚠️ 32-BIT. A curtain wall passes 65,535 vertices easily and a
                // 16-bit buffer wraps silently.
                e.indices32 = true;
                e.vertexCount = verts;
                e.indexCount = uint32_t (upload.indices.size ());
                e.ranges = upload.ranges;
                e.seenThisBatch = true;
                e.hasBounds = true;
                for (int k = 0; k < 3; ++k) {
                    e.boundsMin[k] = upload.boundsMin[k];
                    e.boundsMax[k] = upload.boundsMax[k];
                }
                e.unmappedRanges = 0;
                e.transparentRanges = 0;
                for (const MaterialRange& r : e.ranges) {
                    if (!impl_->materials.Has (r.material))
                        ++e.unmappedRanges;
                    else if (impl_->materials.Lookup (r.material).alpha < kOpaqueAlpha)
                        ++e.transparentRanges;
                }

                if (existing == nullptr)
                    impl_->elements.push_back (std::move (fresh));
                break;
            }

            case SceneCmdType::RemoveElement:
                for (size_t i = 0; i < impl_->elements.size (); ++i) {
                    if (impl_->elements[i].guid != cmd.guid)
                        continue;
                    impl_->elements[i] = std::move (impl_->elements.back ());
                    impl_->elements.pop_back ();
                    break;
                }
                break;

            case SceneCmdType::EndBatch:
                // ⚠️ ONLY A FULL BATCH DROPS THE UNMENTIONED. Getting this
                // backwards on a partial refresh deletes the building and leaves
                // the twelve walls that changed.
                if (impl_->inFullBatch) {
                    for (size_t i = impl_->elements.size (); i-- > 0;) {
                        if (impl_->elements[i].seenThisBatch)
                            continue;
                        impl_->elements[i] = std::move (impl_->elements.back ());
                        impl_->elements.pop_back ();
                    }
                }
                impl_->inFullBatch = false;
                break;

            case SceneCmdType::SetSelection:
                SetSelection (cmd.selection);
                break;
        }
    }

    // ⚠️ THE DEBUG CUBE GOES THE MOMENT THE MODEL ARRIVES. It exists so that an
    // empty project and a broken extraction do not look the same; once there is
    // real geometry it is a two-metre grey box sitting at the project origin,
    // which in an ordinary project is inside the building -- indistinguishable
    // from a genuine extraction fault, and reported as one.
    if (elementsBefore == 0 && !impl_->elements.empty ())
        impl_->staticMeshes.clear ();

    return applied;
}

std::string DiligentScene::GuidForId (uint32_t id) const
{
    if (impl_ == nullptr || id == kNoPickId)
        return std::string ();
    for (const Entry& e : impl_->elements) {
        if (e.pickId == id)
            return e.guid;
    }
    // Ordinary, not an error: the readback lands a few frames after the click
    // and a live sync may have removed the element in between.
    return std::string ();
}

namespace {

// One Entry, as the callout wants it. Free rather than a method on Entry so
// DiligentSceneImpl.hpp stays a description of the shared STATE.
DiligentScene::ElementInfo DescribeEntry (const Entry& e)
{
    DiligentScene::ElementInfo info;
    info.valid = true;
    info.guid = e.guid;
    info.selected = e.selected;
    // ⚠️ hasBounds FALSE IS NOT AN ERROR -- an element uploaded before the
    // extractor started carrying bounds simply has none, and reporting the
    // zeroed array as a real box would put a 0x0x0 element at the origin in the
    // callout, which reads as a broken extraction.
    if (e.hasBounds) {
        for (int i = 0; i < 3; ++i) {
            info.boundsMin[i] = e.boundsMin[i];
            info.boundsMax[i] = e.boundsMax[i];
        }
    }
    info.triangles = e.indexCount / 3;
    info.vertices = e.vertexCount;
    info.materialRanges = e.ranges.size ();
    info.hasTransparency = e.transparentRanges > 0;
    return info;
}

}   // namespace

DiligentScene::ElementInfo DiligentScene::InfoForId (uint32_t id) const
{
    if (impl_ == nullptr || id == kNoPickId)
        return ElementInfo {};
    for (const Entry& e : impl_->elements) {
        if (e.pickId == id)
            return DescribeEntry (e);
    }
    return ElementInfo {};
}

DiligentScene::ElementInfo DiligentScene::InfoForGuid (const std::string& guid) const
{
    if (impl_ == nullptr || guid.empty ())
        return ElementInfo {};
    for (const Entry& e : impl_->elements) {
        if (e.guid == guid)
            return DescribeEntry (e);
    }
    return ElementInfo {};
}

void DiligentScene::SetSelection (const std::vector<std::string>& guids)
{
    if (impl_ == nullptr)
        return;
    impl_->selectionGuids = guids;
    for (Entry& e : impl_->elements)
        e.selected = impl_->IsSelectedGuid (e.guid);
}

// ⚠️ NO LOOP OVER THE ELEMENTS, unlike SetSelection above. The hover is stored
// as the id itself and matched at draw time, so a mouse move costs a single
// store rather than a pass over every element in the project several times a
// second -- and an id belonging to an element that has since left the scene
// simply matches nothing.
void DiligentScene::SetHoverId (uint32_t id)
{
    if (impl_ == nullptr)
        return;
    impl_->hoverId = id;
}

uint32_t DiligentScene::HoverId () const
{
    return impl_ != nullptr ? impl_->hoverId : kNoPickId;
}

// ⚠️ THE FIRST OF THE SET, NOT "THE ONE". Archicad's selection can hold many
// elements and the viewer's own click sets exactly one, so a properties panel has
// to choose. First is the click's element in the case the panel exists for, and
// is at least stable for a multi-element Archicad selection -- which beats
// showing nothing, and beats picking a different member each frame.
std::string DiligentScene::PrimarySelectedGuid () const
{
    if (impl_ == nullptr || impl_->selectionGuids.empty ())
        return std::string ();
    return impl_->selectionGuids.front ();
}

size_t DiligentScene::SelectionCount () const
{
    if (impl_ == nullptr)
        return 0;
    size_t n = 0;
    for (const Entry& e : impl_->elements) {
        if (e.selected)
            ++n;
    }
    return n;
}

bool DiligentScene::SceneBounds (float outMin[3], float outMax[3]) const
{
    bool any = false;
    for (const Entry& e : impl_->elements) {
        if (!e.hasBounds)
            continue;
        for (int k = 0; k < 3; ++k) {
            if (!any || e.boundsMin[k] < outMin[k])
                outMin[k] = e.boundsMin[k];
            if (!any || e.boundsMax[k] > outMax[k])
                outMax[k] = e.boundsMax[k];
        }
        any = true;
    }
    return any;
}

DiligentSceneStats DiligentScene::Stats () const
{
    DiligentSceneStats s;
    if (impl_ == nullptr)
        return s;
    for (const Entry& e : impl_->elements) {
        ++s.elements;
        s.vertices += e.vertexCount;
        s.triangles += e.indexCount / 3;
        s.gpuBytes += e.gpuBytes;
        s.materialMisses += e.unmappedRanges;
        s.transparentRanges += e.transparentRanges;
    }
    for (const Entry& e : impl_->staticMeshes) {
        s.vertices += e.vertexCount;
        s.triangles += e.indexCount / 3;
        s.gpuBytes += e.gpuBytes;
    }
    s.shadowReady = impl_->shadowMap.IsReady ();
    s.shadowFitted = impl_->shadow.valid;
    s.shadowResolution = impl_->shadowMap.Resolution ();
    s.shadowTexelMetres = impl_->shadow.texelWorldSize;

    s.environmentLoaded = impl_->environment.IsLoaded ();
    s.environmentActive = s.environmentLoaded && impl_->environmentEnabled &&
                          impl_->environmentIntensity > 0.0f;
    s.environmentMipLevels = impl_->environment.MipLevels ();
    impl_->environment.AverageRadiance (s.environmentAverage);
    s.environmentPath = impl_->environment.LoadedPath ();
    s.environmentError = impl_->environmentError;
    s.environmentPrefiltered = impl_->environment.IsPrefiltered ();
    s.environmentPrefilteredMips = impl_->environment.PrefilteredMips ();
    s.environmentPrefilterMs = impl_->environment.PrefilterMilliseconds ();
    s.environmentPrefilterError = impl_->environment.PrefilterError ();

    s.autoExposureEnabled = impl_->autoExposureEnabled;
    s.autoExposure = impl_->lastAutoExposure;
    s.sceneLuminance = impl_->lastSceneLuminance;
    s.appliedExposure = impl_->autoExposureEnabled ? impl_->lastAutoExposure : impl_->exposure;
    const WhiteBalanceGains gains =
        ComputeWhiteBalance (impl_->whiteBalanceKelvin, impl_->whiteBalanceTint);
    for (int c = 0; c < 3; ++c)
        s.whiteBalanceGains[c] = gains.rgb[c];
    s.meanAlbedo = MeanPoolAlbedo (impl_->materials);
    for (const SurfaceMaterial& surface : impl_->materials.All ()) {
        const size_t slot = size_t (surface.substance);
        if (slot < 7)
            ++s.substanceCounts[slot];
        if (surface.substance != Substance::Unknown)
            ++s.substanceNamed;
    }
    s.selected = SelectionCount ();
    s.materials = impl_->materials.Size ();
    s.pending = SceneCmdQueue::Get ().PendingCount ();
    s.drawCalls = impl_->drawCalls;
    s.sunApplied = impl_->sunApplied;
    s.sunBelowHorizon = impl_->sunBelowHorizon;
    impl_->EffectiveSun (s.sun);
    s.sunOverridden = impl_->sunOverride;
    constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
    s.sunAzimuthDegrees = impl_->sunOverride
                            ? impl_->sunOverrideAzimuth
                            : std::atan2 (impl_->sun[1], impl_->sun[0]) * kRadToDeg;
    s.northDegrees = impl_->northDegrees;
    float bearing = impl_->northDegrees - s.sunAzimuthDegrees;
    bearing -= 360.0f * std::floor (bearing / 360.0f);
    s.sunBearingDegrees = bearing;
    s.sunAltitudeDegrees = impl_->sunOverride
                             ? impl_->sunOverrideAltitude
                             : std::asin (impl_->sun[2] < -1.0f ? -1.0f
                                          : (impl_->sun[2] > 1.0f ? 1.0f : impl_->sun[2]))
                               * kRadToDeg;
    s.ambient = impl_->ambient;
    s.latitudeDegrees = impl_->latitudeDegrees;
    s.longitudeDegrees = impl_->longitudeDegrees;
    s.siteAltitudeMetres = impl_->siteAltitudeMetres;
    s.year = impl_->year;
    s.month = impl_->month;
    s.day = impl_->day;
    s.hour = impl_->hour;
    s.minute = impl_->minute;
    s.summerTime = impl_->summerTime;
    s.haveComputedSun = impl_->haveComputedSun;
    s.computedAzimuthDegrees = impl_->computedAzimuthDegrees;
    s.computedAltitudeDegrees = impl_->computedAltitudeDegrees;
    return s;
}

}   // namespace archviz
}   // namespace geomsrv
