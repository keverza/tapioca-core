#ifndef EVP_ARCHVIZ_EXTRACTIONSCENECACHE_HPP
#define EVP_ARCHVIZ_EXTRACTIONSCENECACHE_HPP

// ArchViz/ExtractionSceneCache -- the extraction worker's use of the per-project scene cache (#14).
//
// The format and the rules are ProjectSceneCache.hpp; the main-thread readings are
// SceneCacheFingerprint.hpp. This is the order they happen in, inside a FULL pass:
//
//   BeginFullPass  identity (one gate hop) -> load the file -> on the session's first pass,
//                  stream it to the scene: BeginBatch(full), SetMaterials, SetEnvironment,
//                  upserts, EndBatch -- no capture, and NOT AN OCCLUDER (below)
//   Prepare        after the acquire: remap the cached pool to the live one by identity; the
//                  slices' reuse index holds only elements whose every index maps
//   HarvestSlice   between slices: re-send each reused element from the cache down the path a
//                  captured one takes (story cut, snapshot, upsert), and note what is stored next
//   FinishFullPass after EndBatch: reconcile, log, write when something changed
//   Refine         after the texture pass (ExtractionDeferredUv): store the coordinates it read
//
// ⚠️ THE CACHED BATCH NEVER FEEDS THE OVERLAY'S HOST OCCLUDER. The overlay exists to agree with
// Archicad (ArchVizPanel.cpp, OVERLAY-INVARIANTS.md §5-§6): yesterday's building occluding over a
// live 3D window answers that question wrongly. The live pass right behind it feeds the occluder
// as it always did, and is also the only one that publishes a snapshot.
//
// ⚠️ THE CACHE IS FOR THE VIEWER PANEL ONLY, opted in with SetEnabled (ArchVizPanel.cpp). Every
// other full pass -- the overlay's, a capture's, the sun-study follower's -- is untouched while it
// is off, and it is off by default.
//
// WORKER THREAD, all of it, except SetEnabled (any thread).

#include "ArchViz/ExtractionSlice.hpp" // SliceCache, SliceState
#include "ArchViz/ProjectSceneCache.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace geomsrv {
class SnapshotAssembly;
namespace archviz {

class StorySliceAccumulator;

class ExtractionSceneCache final {
  public:
    static ExtractionSceneCache& Get ();

    // On with the viewer panel, off at its close: off forgets the held scene and the session.
    void SetEnabled (bool on);
    bool Enabled () const
    {
        return enabled_.load ();
    }

    void BeginFullPass (const std::atomic<bool>& stop);
    // Null when the cache is off or the project has none.
    std::shared_ptr<const extractionslice::SliceCache>
    Prepare (const std::map<int32_t, CapturedMaterialIdentity>& livePool);
    // Returns the uploads it pushed; `triangles` and `packetsComplete` as the pass's own loop keeps them.
    uint32_t HarvestSlice (const extractionslice::SliceState& st, SnapshotAssembly& assembly,
                           StorySliceAccumulator& storeys, std::vector<SurfaceSubstanceObservation>& observations,
                           std::vector<std::string>* extractedGuids, uint64_t& triangles, bool& packetsComplete);
    void NoteMaterials (const MaterialTable& table);
    void NoteEnvironment (const EnvironmentUpload& environment);
    void FinishFullPass (const std::shared_ptr<const Snapshot>& snapshot, bool complete);
    void Refine (const std::shared_ptr<const Snapshot>& snapshot, const std::set<std::string>& refined);

  private:
    ExtractionSceneCache () = default;

    struct Pass {
        bool active = false;
        std::string project;
        std::shared_ptr<const scenecache::State> source;
        scenecache::PoolRemap remap;
        std::unordered_map<std::string, size_t> meshIndex; // reusable GUID -> source mesh
        std::map<std::string, scenecache::ElementRecord> records;
        std::vector<std::string> reused, captured;
        std::shared_ptr<const MaterialTable> materials;
        bool haveEnvironment = false;
        EnvironmentUpload environment;
        uint32_t cheapHits = 0, deepHits = 0, deepReads = 0, unmappable = 0;
        double readMs = 0.0;
        int64_t startedMs = 0;
    };

    void Preload (const scenecache::State& state, const std::atomic<bool>& stop);
    void Store (const scenecache::State& state, const char* why);

    std::atomic<bool> enabled_ { false };
    std::mutex mutex_; // held_, preloaded_, awaitingUv_
    std::shared_ptr<const scenecache::State> held_;
    bool preloaded_ = false;
    // Textured elements stored before their coordinates were read: their fingerprints wait here,
    // and are stored as "never reuse" until Refine brings the coordinates.
    std::map<std::string, scenecache::Fingerprint> awaitingUv_;
    std::string announcedNone_; // the last "no cache for this project" reason logged
    Pass pass_;                 // worker only
};

} // namespace archviz
} // namespace geomsrv

#endif
