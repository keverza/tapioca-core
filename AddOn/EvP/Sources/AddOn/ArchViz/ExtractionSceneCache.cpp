// ArchViz/ExtractionSceneCache -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionSceneCache.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ElementPacket.hpp"
#include "ArchViz/ExtractionDeferredUv.hpp"
#include "ArchViz/ExtractionStorySlices.hpp"
#include "ArchViz/SceneCacheFingerprint.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "Geometry/SnapshotAssembly.hpp"
#include "Python/MainThreadGate.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>

namespace geomsrv {
namespace archviz {

namespace {

constexpr int kIdentityTimeoutMs = 1500;
// The preload throttles on the same budget as the pass (ExtractionThread.cpp, MaxPendingBytes).
constexpr size_t kMaxPendingBytes = 96u * 1024u * 1024u;

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

std::filesystem::path CacheDirectory ()
{
    const wchar_t* root = _wgetenv (L"LOCALAPPDATA");
    if (root == nullptr || *root == 0)
        return {};
    return std::filesystem::path (root) / "Tapioca" / "SceneCache";
}

std::string Mb (uint64_t bytes)
{
    char text[32] = {};
    std::snprintf (text, sizeof (text), "%.1f MB", double (bytes) / (1024.0 * 1024.0));
    return text;
}

// One element down the path a captured one takes. False when it yields no upload.
bool SendElement (Mesh mesh, SnapshotAssembly* assembly, StorySliceAccumulator* storeys, bool feedOccluder,
                  uint64_t& triangles)
{
    if (storeys != nullptr)
        storeys->Cut (mesh);
    CapturedMeshPacket packet;
    packet.capturedAt = std::chrono::steady_clock::now ();
    packet.mesh = std::move (mesh);
    std::unique_ptr<ElementUpload> up = MakeElementPacket (packet);
    if (assembly != nullptr)
        assembly->Add (std::move (packet.mesh));
    if (up == nullptr)
        return false;
    triangles += up->indices.size () / 3;
    SceneCmdQueue::Get ().PushUpsert (std::move (up), feedOccluder);
    return true;
}

} // namespace

ExtractionSceneCache& ExtractionSceneCache::Get ()
{
    static ExtractionSceneCache instance;
    return instance;
}

void ExtractionSceneCache::SetEnabled (bool on)
{
    const bool was = enabled_.exchange (on);
    if (was == on)
        return;
    std::lock_guard<std::mutex> lock (mutex_);
    // ⚠️ A NEW SESSION STARTS CLEAN (OVERLAY-INVARIANTS.md §8): the next open loads and preloads again.
    held_.reset ();
    awaitingUv_.clear ();
    preloaded_ = false;
    announcedNone_.clear ();
}

void ExtractionSceneCache::BeginFullPass (const std::atomic<bool>& stop)
{
    pass_ = Pass {};
    if (!enabled_.load ())
        return;
    pass_.startedMs = NowMs ();

    auto identity = std::make_shared<std::string> ();
    GS::UniString err;
    if (!evp::MainThreadGate::Get ().InvokeUnlessStopped (
            [identity] { *identity = scenecache::ReadProjectIdentity (); }, kIdentityTimeoutMs, stop, err) ||
        identity->empty ()) {
        const std::string why = identity->empty () ? "untitled project or no project path" : "gate timed out";
        if (why != announcedNone_)
            ArchVizLog ("scene cache: none for this pass (" + why + ")");
        announcedNone_ = why;
        return;
    }
    announcedNone_.clear ();
    const std::filesystem::path directory = CacheDirectory ();
    if (directory.empty ())
        return;

    std::shared_ptr<const scenecache::State> held;
    bool preload = false;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        if (held_ != nullptr && held_->project != *identity) {
            held_.reset (); // another project: its scene is not this one's
            awaitingUv_.clear ();
            preloaded_ = false;
        }
        held = held_;
    }

    if (held == nullptr) {
        const std::filesystem::path file = scenecache::FileFor (directory, *identity);
        const int64_t started = NowMs ();
        std::string bytes;
        std::error_code ec;
        if (std::filesystem::exists (file, ec) && scenecache::ReadWhole (file, bytes)) {
            auto state = std::make_shared<scenecache::State> ();
            const scenecache::DecodeStatus status = scenecache::Decode (bytes, *identity, *state);
            if (status == scenecache::DecodeStatus::Ok) {
                std::filesystem::last_write_time (file, std::filesystem::file_time_type::clock::now (), ec); // LRU
                ArchVizLog ("scene cache: loaded " + std::to_string (state->geometry->meshes.size ()) + " elements, " +
                            Mb (bytes.size ()) + " in " + std::to_string (NowMs () - started) + " ms from " +
                            file.filename ().string ());
                held = state;
                std::lock_guard<std::mutex> lock (mutex_);
                held_ = held;
            }
            else {
                // ⚠️ REFUSED WHOLE, AND DELETED: the next complete pass writes a good one.
                std::filesystem::remove (file, ec);
                ArchVizLog (std::string ("scene cache: refused ") + file.filename ().string () + " (" +
                            scenecache::DescribeStatus (status) + "); deleted, rebuilt by this pass");
            }
        }
        else
            ArchVizLog ("scene cache: no file yet for this project; this pass writes it");
    }
    {
        std::lock_guard<std::mutex> lock (mutex_);
        preload = held != nullptr && !preloaded_;
        if (held != nullptr)
            preloaded_ = true;
    }
    if (preload && SceneCmdQueue::Get ().HasConsumer ())
        Preload (*held, stop);

    pass_.active = true;
    pass_.project = *identity;
    pass_.source = held;
}

void ExtractionSceneCache::Preload (const scenecache::State& state, const std::atomic<bool>& stop)
{
    const int64_t started = NowMs ();
    // ⚠️ THE ORDER OF EVERY BATCH: BeginBatch, the table, the sun, the geometry (SceneCmdQueue.hpp).
    // FULL, so the live pass behind it -- also full -- replaces it element by element and drops
    // whatever it no longer finds, with no empty frame between the two.
    SceneCmdQueue& queue = SceneCmdQueue::Get ();
    queue.PushBeginBatch (true, 0, /*feedOccluder*/ false);
    if (state.materials != nullptr)
        queue.PushMaterials (std::make_unique<MaterialTable> (*state.materials));
    if (state.haveEnvironment)
        queue.PushEnvironment (state.environment);
    uint32_t sent = 0;
    uint64_t triangles = 0;
    for (const Mesh& mesh : state.geometry->meshes) {
        while (queue.PendingBytes () > kMaxPendingBytes && !stop.load ())
            std::this_thread::sleep_for (std::chrono::milliseconds (16));
        if (stop.load ())
            break;
        if (SendElement (mesh, nullptr, nullptr, /*feedOccluder*/ false, triangles))
            ++sent;
    }
    // ⚠️ NO CAPTURE: what the cache holds is not yet what Archicad holds, so nothing is published.
    queue.PushEndBatch (nullptr, /*feedOccluder*/ false);
    ArchVizLog ("pipeline: stage=scene-cache-preload elements=" + std::to_string (sent) +
                " triangles=" + std::to_string (triangles) + " wallMs=" + std::to_string (NowMs () - started) +
                " published=0 occluder=0");
}

std::shared_ptr<const extractionslice::SliceCache>
ExtractionSceneCache::Prepare (const std::map<int32_t, CapturedMaterialIdentity>& livePool)
{
    if (!pass_.active)
        return nullptr;
    auto cache = std::make_shared<extractionslice::SliceCache> ();
    cache->pool = std::make_shared<const std::map<int32_t, CapturedMaterialIdentity>> (livePool);
    if (pass_.source == nullptr || livePool.empty ())
        return cache; // nothing to reuse; every captured element is still fingerprinted
    const scenecache::State& source = *pass_.source;
    pass_.remap = scenecache::BuildPoolRemap (source.geometry->materialIdentity, livePool);
    auto reuse = std::make_shared<scenecache::ReuseIndex> ();
    reuse->reserve (source.geometry->meshes.size ());
    for (size_t i = 0; i < source.geometry->meshes.size (); ++i) {
        const Mesh& mesh = source.geometry->meshes[i];
        const auto record = source.records.find (mesh.guid);
        if (record == source.records.end () || record->second.fingerprint.shape == 0)
            continue;
        if (!scenecache::CanRemap (mesh, record->second, pass_.remap)) {
            ++pass_.unmappable;
            continue;
        }
        (*reuse)[mesh.guid] = record->second.fingerprint;
        pass_.meshIndex[mesh.guid] = i;
    }
    cache->reuse = std::move (reuse);
    ArchVizLog ("scene cache: " + std::to_string (pass_.meshIndex.size ()) + " of " +
                std::to_string (source.geometry->meshes.size ()) + " cached elements eligible; surface pool " +
                (pass_.remap.identity ? std::string ("unchanged")
                                      : "renumbered (" + std::to_string (pass_.remap.to.size ()) + " mapped, " +
                                            std::to_string (pass_.remap.unmapped.size ()) + " unmapped)") +
                ", " + std::to_string (pass_.unmappable) + " elements re-captured for it");
    return cache;
}

uint32_t ExtractionSceneCache::HarvestSlice (const extractionslice::SliceState& st, SnapshotAssembly& assembly,
                                             StorySliceAccumulator& storeys,
                                             std::vector<SurfaceSubstanceObservation>& observations,
                                             std::vector<std::string>* extractedGuids, uint64_t& triangles,
                                             bool& packetsComplete)
{
    if (!pass_.active)
        return 0;
    pass_.cheapHits += st.cheapHits;
    pass_.deepHits += st.deepHits;
    pass_.deepReads += st.deepReads;
    pass_.readMs += st.readMs;
    for (const extractionslice::CacheVisit& visit : st.captured) {
        scenecache::ElementRecord& record = pass_.records[visit.guid];
        record.fingerprint = visit.fingerprint;
        record.observations.assign (st.observations.begin () + std::ptrdiff_t (visit.observationsFrom),
                                    st.observations.begin () + std::ptrdiff_t (visit.observationsTo));
        pass_.captured.push_back (visit.guid);
    }
    uint32_t pushed = 0;
    for (const extractionslice::CacheVisit& visit : st.reused) {
        const auto index = pass_.meshIndex.find (visit.guid);
        if (index == pass_.meshIndex.end () || pass_.source == nullptr) {
            packetsComplete = false; // cannot happen: the slice reuses only what Prepare indexed
            continue;
        }
        Mesh mesh = pass_.source->geometry->meshes[index->second];
        scenecache::ElementRecord record { visit.fingerprint, pass_.source->records.at (visit.guid).observations };
        scenecache::Remap (mesh, record.observations, pass_.remap);
        observations.insert (observations.end (), record.observations.begin (), record.observations.end ());
        if (extractedGuids != nullptr)
            extractedGuids->push_back (visit.guid);
        pass_.records[visit.guid] = std::move (record);
        pass_.reused.push_back (visit.guid);
        if (SendElement (std::move (mesh), &assembly, &storeys, /*feedOccluder*/ true, triangles))
            ++pushed;
        else
            packetsComplete = false;
    }
    return pushed;
}

void ExtractionSceneCache::NoteMaterials (const MaterialTable& table)
{
    if (pass_.active)
        pass_.materials = std::make_shared<const MaterialTable> (table);
}

void ExtractionSceneCache::NoteEnvironment (const EnvironmentUpload& environment)
{
    if (!pass_.active)
        return;
    pass_.haveEnvironment = true;
    pass_.environment = environment;
}

void ExtractionSceneCache::FinishFullPass (const std::shared_ptr<const Snapshot>& snapshot, bool complete)
{
    if (!pass_.active)
        return;
    Pass pass = std::move (pass_);
    pass_ = Pass {};

    std::set<std::string> cached;
    if (pass.source != nullptr)
        for (const auto& entry : pass.source->records)
            cached.insert (entry.first);
    const scenecache::Reconciliation r = scenecache::Reconcile (cached, pass.reused, pass.captured);
    ArchVizLog ("pipeline: stage=scene-cache-reconcile reused=" + std::to_string (r.reused.size ()) +
                " cheap=" + std::to_string (pass.cheapHits) + " deep=" + std::to_string (pass.deepHits) +
                " deepReads=" + std::to_string (pass.deepReads) + " changed=" + std::to_string (r.changed.size ()) +
                " appeared=" + std::to_string (r.appeared.size ()) + " stale=" + std::to_string (r.stale.size ()) +
                " unmappable=" + std::to_string (pass.unmappable) +
                " poolRenumbered=" + std::to_string (pass.source != nullptr && !pass.remap.identity) +
                " readMs=" + std::to_string (int64_t (pass.readMs)) +
                " passMs=" + std::to_string (NowMs () - pass.startedMs) + " complete=" + std::to_string (complete));
    if (!complete || snapshot == nullptr) {
        ArchVizLog ("scene cache: not stored -- the pass did not complete");
        return;
    }

    scenecache::State next;
    next.project = pass.project;
    next.geometry = snapshot;
    next.materials =
        pass.materials != nullptr ? pass.materials : (pass.source != nullptr ? pass.source->materials : nullptr);
    next.haveEnvironment = pass.haveEnvironment;
    next.environment = pass.environment;
    // ⚠️ A TEXTURED ELEMENT DRAWN BEFORE ITS COORDINATES (ExtractionDeferredUv.hpp) IS STORED AS
    // "NEVER REUSE" until Refine brings them: reused as it is, it would stay untextured for good,
    // because only a capture asks for the texture pass.
    const std::set<std::string> untextured = extractiondeferreduv::Peek ();
    std::map<std::string, scenecache::Fingerprint> awaiting;
    for (const Mesh& mesh : snapshot->meshes) {
        const auto found = pass.records.find (mesh.guid);
        if (found == pass.records.end ())
            continue; // no reading: stored without one, so never reused
        scenecache::ElementRecord record = found->second;
        if (untextured.count (mesh.guid) != 0) {
            awaiting[mesh.guid] = record.fingerprint;
            record.fingerprint = {};
        }
        next.records.emplace (mesh.guid, std::move (record));
    }
    const bool materialsMoved = pass.source == nullptr || pass.source->materials == nullptr ||
                                next.materials == nullptr ||
                                pass.source->materials->Size () != next.materials->Size () ||
                                pass.source->materials->Textures ().size () != next.materials->Textures ().size ();
    const bool changed = pass.source == nullptr || r.Any () || !pass.remap.identity || pass.unmappable > 0 ||
                         !awaiting.empty () || materialsMoved;
    if (changed)
        Store (next, pass.source == nullptr ? "first pass" : "the model changed since the cache");
    else
        ArchVizLog ("scene cache: unchanged, not rewritten");

    std::lock_guard<std::mutex> lock (mutex_);
    if (enabled_.load ()) {
        held_ = std::make_shared<const scenecache::State> (std::move (next));
        awaitingUv_ = std::move (awaiting);
    }
}

void ExtractionSceneCache::Refine (const std::shared_ptr<const Snapshot>& snapshot,
                                   const std::set<std::string>& refined)
{
    if (!enabled_.load () || snapshot == nullptr || !snapshot->completeModel || refined.empty ())
        return;
    std::shared_ptr<scenecache::State> next;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        if (held_ == nullptr || awaitingUv_.empty ())
            return;
        next = std::make_shared<scenecache::State> (*held_);
        next->geometry = snapshot; // the same capture, its textured elements now with coordinates
        for (const std::string& guid : refined) {
            const auto waiting = awaitingUv_.find (guid);
            const auto record = next->records.find (guid);
            if (waiting == awaitingUv_.end () || record == next->records.end ())
                continue;
            record->second.fingerprint = waiting->second;
            awaitingUv_.erase (waiting);
        }
    }
    Store (*next, "texture coordinates read");
    std::lock_guard<std::mutex> lock (mutex_);
    if (enabled_.load () && held_ != nullptr && held_->project == next->project)
        held_ = std::move (next);
}

void ExtractionSceneCache::Store (const scenecache::State& state, const char* why)
{
    const std::filesystem::path directory = CacheDirectory ();
    if (directory.empty () || state.project.empty ())
        return;
    const int64_t started = NowMs ();
    const std::string bytes = scenecache::Encode (state);
    const int64_t encoded = NowMs ();
    if (bytes.size () > scenecache::kMaxFileBytes) {
        ArchVizLog ("scene cache: not stored -- " + Mb (bytes.size ()) + " is over the per-project cap of " +
                    Mb (scenecache::kMaxFileBytes));
        return;
    }
    const std::filesystem::path file = scenecache::FileFor (directory, state.project);
    std::string error;
    if (!scenecache::WriteAtomic (file, bytes, error)) {
        ArchVizLog ("scene cache: not stored -- " + error);
        return;
    }
    const size_t evicted = scenecache::Evict (directory, scenecache::kMaxTotalBytes, file);
    ArchVizLog ("pipeline: stage=scene-cache-store elements=" + std::to_string (state.geometry->meshes.size ()) +
                " bytes=" + std::to_string (bytes.size ()) + " encodeMs=" + std::to_string (encoded - started) +
                " writeMs=" + std::to_string (NowMs () - encoded) + " evicted=" + std::to_string (evicted) +
                " reason=\"" + why + "\"");
}

} // namespace archviz
} // namespace geomsrv
