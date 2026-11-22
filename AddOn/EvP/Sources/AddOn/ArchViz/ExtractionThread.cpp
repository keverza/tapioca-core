#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ExtractionReport.hpp"
#include "ArchViz/ExtractionSlice.hpp"       // SliceState, Run -- one slice's work
#include "ArchViz/ExtractionDeferredUv.hpp"  // the texture pass refines its capture (#14)
#include "ArchViz/ExtractionSceneCache.hpp"  // the viewer's per-project scene on disk (#14)
#include "ArchViz/ArchVizLog.hpp"            // ArchVizLog
#include "ArchViz/HudConsole.hpp"            // the Debug tab's console: what the user checks when something fails
#include "ArchViz/ExtractionEnvironment.hpp" // ReadMaterials, ReadEnvironment
#include "ArchViz/ExtractionSubstance.hpp"   // ReadProjectSubstances, ObserveElementSubstances
#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/MeshGroups.hpp"
#include "ArchViz/ElementPacket.hpp"
#include "ArchViz/ExtractionStorySlices.hpp" // ReadStoreys, StorySliceAccumulator
#include "ArchViz/SceneCmdQueue.hpp"
#include "Geometry/GeometryExtractor.hpp"
#include "Geometry/SnapshotAssembly.hpp"
#include "Geometry/MeshStore.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "Python/MainThreadGate.hpp"

#include <Model.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace geomsrv {
namespace archviz {

namespace {

using extractionslice::SliceState;

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

using extractionslice::SliceTimeoutMs; // the worst-case freeze on close

// Acquiring the model can force the 3D model to GENERATE. That is the one long
// call in the pass and it cannot be sliced (same shape as BackgroundArm's
// listing), so it gets a timeout of its own.
//
// ⚠️ AND IT IS WHY A REFRESH IS NOT FREE. On a project whose 3D has never been
// built this is tens of seconds of Archicad working — during which Archicad IS
// blocked, because the generation happens inside our gate slice. There is no way
// around that with this API; what there is, is not doing it implicitly. Nothing
// starts a pass on its own: EvP.ViewerRefresh does, because the user asked.
//
// ⚠️ 20 s, NOT 120 — MEASURED THE HARD WAY 2026-08-07. Starting live sync from a
// 2D window froze Archicad with a busy cursor and NOTHING could stop it: the
// worker was inside this Invoke, `Stop()` joins the worker, and the join
// therefore could not return for up to two minutes. The timeout IS the
// worst-case freeze a user cannot escape, so it is now short enough to sit
// through. A generation that needs longer than this is not abandoned quietly —
// the pass says exactly what happened and what to do about it.
constexpr int AcquireTimeoutMs = 20000;

// A pass whose model changed under it starts again this long after, at most this often.
constexpr int64_t kRestartSettleMs = 500;
constexpr uint32_t kMaxRestarts = 8;

// Stop queueing when this much is waiting for the GPU. Peak memory is otherwise
// roughly the whole snapshot twice (SceneCmdQueue.hpp's ⚠️ on being unbounded),
// and on a large project the extractor easily outruns a 32-command-per-frame
// consumer.
//
// 96 MiB is about 1.5 M vertices in flight — far more than a frame's worth,
// small enough that the viewer's memory does not track the project's size.
constexpr size_t MaxPendingBytes = 96u * 1024u * 1024u;

// The model, and the fact that it must die on the main thread.
//
// ⚠️ RAW POINTER ON PURPOSE. `ModelerAPI::Model` is a DevKit object whose
// destructor is DevKit code, so it may not run on this thread — a shared_ptr
// would put its deleter wherever the last reference happened to go, which is
// exactly the accident this file cannot afford. The pointer is created inside a
// gate slice and deleted inside a gate slice, and the worker only ever passes it
// along.
struct ModelHandle {
    ModelerAPI::Model* model = nullptr;
    int32_t count = 0;
    double captureMilliseconds = 0.0;
    uint64_t captureStamp = 0;
    std::map<int32_t, double> transparency;
    std::map<int32_t, CapturedMaterialIdentity> materialIdentity;
    std::map<int32_t, CapturedSurface> surfaceAttributes;

    // ⚠️ SET WHEN THE ACQUIRE TIMES OUT, AND THE LAMBDA MUST CHECK IT. A gate
    // Invoke that reports a timeout may STILL RUN LATER — the contract says so
    // — and by then the pass that asked for the model has given up. Without this
    // the late job would hand a freshly generated model to nobody and leak it,
    // once per abandoned refresh. Checked on the MAIN thread, inside the job, so
    // the delete happens where it is legal.
    std::atomic<bool> abandoned { false };
};

} // namespace

ExtractionWorker& ExtractionWorker::Get ()
{
    static ExtractionWorker instance;
    return instance;
}

ExtractionWorker::~ExtractionWorker ()
{
    Stop ();
}

void ExtractionWorker::Start (bool full, int64_t sliceMs, int64_t gapMs, int64_t maxSeconds)
{
    Options opt;
    opt.mode = Mode::Once;
    opt.full = full;
    opt.sliceMs = sliceMs;
    opt.gapMs = gapMs;
    opt.maxSeconds = maxSeconds;
    StartWith (opt);
}

void ExtractionWorker::StartUpdate (const std::set<std::string>& guids, int64_t sliceMs, int64_t gapMs)
{
    Options opt;
    opt.mode = Mode::Once;
    opt.full = false;
    opt.sliceMs = sliceMs;
    opt.gapMs = gapMs;
    opt.only = guids;
    StartWith (opt);
}

void ExtractionWorker::StartLive (int64_t settleMs, int64_t pollMs, int64_t sliceMs, int64_t gapMs, bool armObservers)
{
    Options opt;
    opt.mode = Mode::Live;
    opt.armObservers = armObservers;
    opt.full = true; // live sync begins from a known-complete scene
    opt.sliceMs = sliceMs;
    opt.gapMs = gapMs;
    opt.settleMs = settleMs;
    opt.pollMs = pollMs;
    // No wall clock: a watch loop is meant to run for as long as the viewer is
    // open. The per-PASS budget below is what bounds any single re-extraction.
    opt.maxSeconds = 24 * 60 * 60;
    StartWith (opt);
}

void ExtractionWorker::StartWith (const Options& opt)
{
    Stop (); // a restart supersedes the pass in flight

    stopFlag_.store (false);
    running_.store (true);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_ = Progress ();
        progress_.running = true;
        progress_.live = (opt.mode == Mode::Live);
        progress_.phase = "starting";
    }

    worker_ = std::thread ([this, opt] { Run (opt); });
}

void ExtractionWorker::RequestStop ()
{
    stopFlag_.store (true);
}

void ExtractionWorker::Stop ()
{
    stopFlag_.store (true);
    if (worker_.joinable ())
        worker_.join (); // bounded by SliceTimeoutMs — see the header
    running_.store (false);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.running = false;
        progress_.live = false;
    }
}

ExtractionWorker::Progress ExtractionWorker::Snapshot () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return progress_;
}

void ExtractionWorker::Run (Options opt)
{
    std::vector<std::string> extracted;
    const bool ok = RunPass (opt, opt.only.empty () && opt.full, opt.only, opt.only.empty () ? &extracted : nullptr);

    if (opt.mode == Mode::Live && ok && !stopFlag_.load ()) {
        // ⚠⚠ NO OBSERVERS, NO WATCH LOOP — AND THE SECOND HALF IS AS IMPORTANT AS
        // THE FIRST (PLAT-RE68). Arming writes to the project database (see
        // StartLive), so it is off by default; but RunLiveLoop's idle branch
        // treats "not watching" as a DROPPED watch and answers it with a FULL
        // re-extraction, so leaving the loop running with nothing armed would
        // re-extract the whole model forever. One pass, then stop, and say so.
        if (!opt.armObservers) {
            extractionreport::SinglePassNotice ();
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.phase = "one pass (live refresh disabled: see archviz.log)";
        }
        else {
            ArmObservers (extracted, opt);
            RunLiveLoop (opt);
        }
    }

    running_.store (false);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.running = false;
        progress_.live = false;
    }
}

// ---------------------------------------------------------------------------
// ONE PASS. `filter` empty = the whole model; otherwise only those GUIDs, and
// anything named in the filter that the model does NOT contain is pushed as a
// REMOVAL — which is how a deleted element leaves the scene without anything
// having to interpret Archicad's event ids.
// ---------------------------------------------------------------------------
bool ExtractionWorker::RunPass (const Options& opt, bool full, const std::set<std::string>& filter,
                                std::vector<std::string>* extractedGuids)
{
    const int64_t started = NowMs ();
    bool partial = !filter.empty ();
    std::set<std::string> effectiveFilter = filter;
    const MeshStore::FullPassScope inFlight (!partial);
    auto baseSnapshot = lastSceneCapture_.lock ();
    if (!baseSnapshot)
        baseSnapshot = MeshStore::Get ().Shared ();

    const auto fail = [this, started] (const std::string& why, bool console = true) {
        {
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.phase = why;
            progress_.elapsedMs = NowMs () - started;
        }
        ArchVizLog ("extraction: " + why);
        if (console)
            hudconsole::Error ("Model", "not read: " + why);
    };

    {
        // Per-pass counters reset; the cumulative ones (fullPasses, removed,
        // armed) deliberately do not — over a long live session those are the
        // history, and resetting them would make the viewer look like it had
        // just started every time a wall moved.
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.total = progress_.extracted = progress_.empty = progress_.pushed = 0;
        progress_.emptyByType.clear ();
        progress_.emptyReasons.clear ();
        progress_.triangles = 0;
        progress_.slices = 0;
        progress_.longestHoldMs = progress_.longestRoundTripMs = 0;
        progress_.acquireMs = progress_.throttledMs = 0;
        progress_.done = progress_.gaveUp = false;
        progress_.phase = "acquiring";
    }

    // ---- phase 1: acquire the model, the surface pool and the sun -----------
    // ONE slice, because AcquireCurrentModel is one uninterruptible call. The
    // pool and the sun ride along: both are small, both are ModelerAPI/ACAPI,
    // and a second gate hop for them would cost a round trip to save nothing.
    auto handle = std::make_shared<ModelHandle> ();
    auto materials = std::make_shared<std::unique_ptr<MaterialTable>> ();
    auto env = std::make_shared<EnvironmentUpload> ();
    auto haveEnv = std::make_shared<std::atomic<bool>> (false);
    // RE51: the project's building materials, classified. Read here with the
    // surface pool for exactly the same reason -- both are small, both are
    // per-project rather than per-element, and a second gate hop would cost a
    // round trip to save nothing. Only a FULL pass needs it.
    auto substances = std::make_shared<ProjectSubstances> ();
    // The storeys ride along for the same reason the sun does: small, per-project,
    // and a second gate hop would cost a round trip to save nothing.
    auto storeys = std::make_shared<ProjectStoreys> ();
    // ⚠️ READ ONCE, HERE. Re-reading per element would let a mid-pass toggle
    // union a storey against only the elements reached so far. The plan view's
    // cut likewise (SetPlanCut).
    const bool wantStorySlices = storySlicesWanted_.load ();
    double planCutZ = 0.0;
    const bool wantPlanCut = PlanCut (planCutZ);

    // #14: the viewer's cached scene, before the model is asked for -- drawn while Archicad builds it.
    ExtractionSceneCache& sceneCache = ExtractionSceneCache::Get ();
    if (full && !partial)
        sceneCache.BeginFullPass (stopFlag_);

    const int64_t acquireStart = NowMs ();
    GS::UniString gateErr;
    evp::gatewait::Outcome acquireOutcome = evp::gatewait::Outcome::TimedOut; // Abandoned: superseded, not slow
    const bool acquired = evp::MainThreadGate::Get ().InvokeUnlessStopped (
        [handle, materials, env, haveEnv, substances, storeys, full, wantStorySlices] {
            const auto started = std::chrono::steady_clock::now ();
            auto model = std::make_unique<ModelerAPI::Model> ();
            if (!AcquireCurrentModel (*model))
                return; // handle->model stays null: "no 3D model"

            // ⚠️ BEFORE ANYTHING ELSE IS DONE WITH IT. If the pass gave up while
            // Archicad was generating, this job is now the model's only owner
            // and the only thread allowed to destroy it. Letting the unique_ptr
            // go out of scope here does exactly that, on the main thread.
            if (handle->abandoned.load ())
                return;

            handle->count = ModelElementCount (*model);
            handle->captureStamp = modelwatch::CaptureStamp ();
            handle->transparency = ReadMaterialTransparency (*model);
            handle->materialIdentity = ReadMaterialIdentity (*model);
            handle->surfaceAttributes = ReadSurfaceAttributes ();
            *materials = ReadMaterials (*model);
            if (full)
                *substances = ReadProjectSubstances ();
            if (full && wantStorySlices) // FULL only - see StorySliceAccumulator
                *storeys = ReadStoreys ();
            haveEnv->store (ReadEnvironment (*env));
            handle->model = model.release (); // ⚠️ ownership moves to the pass
            handle->captureMilliseconds =
                std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
        },
        AcquireTimeoutMs, stopFlag_, gateErr, &acquireOutcome);
    const int64_t acquireMs = NowMs () - acquireStart;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.acquireMs = acquireMs;
        progress_.longestHoldMs = std::max (progress_.longestHoldMs, acquireMs);
        progress_.longestRoundTripMs = std::max (progress_.longestRoundTripMs, acquireMs);
    }

    if (!acquired) {
        // ⚠️ ABANDON IT PROPERLY. The job may still be running (Archicad is
        // generating the 3D model right now) and may finish after this line; the
        // flag is what tells it to destroy what it built instead of leaking it.
        handle->abandoned.store (true);
        const bool stopped = acquireOutcome == evp::gatewait::Outcome::Abandoned; // no console error
        fail (extractionslice::AcquireFailure (stopped, AcquireTimeoutMs, acquireMs), !stopped);
        return false;
    }
    if (handle->model == nullptr) {
        fail ("this project has no 3D model to show - open the 3D window once, then refresh");
        return false;
    }
    ArchVizLog ("pipeline: stage=geometry-acquire elements=" + std::to_string (handle->count) +
                " hostCaptureMs=" + std::to_string (handle->captureMilliseconds) +
                " gateRoundTripMs=" + std::to_string (acquireMs) + " full=" + std::to_string (full));

    const bool refining = partial && extractiondeferreduv::Suspended (); // same capture, no edit
    bool abandon = false; // a refinement the model moved under: given up, never a full pass
    if (partial && (handle->materialIdentity.empty () || handle->surfaceAttributes.empty () ||
                    !SnapshotAssembly::CanUpdate (baseSnapshot.get (), handle->captureStamp, handle->transparency,
                                                  handle->materialIdentity, handle->surfaceAttributes, refining)))
        abandon = !extractiondeferreduv::FallBackToFull (refining, effectiveFilter, partial, full);
    SnapshotAssembly assembly (MeshStore::Get ().NextId (), handle->captureStamp, handle->transparency,
                               partial ? baseSnapshot : nullptr, effectiveFilter, handle->materialIdentity,
                               handle->surfaceAttributes, refining);

    StorySliceAccumulator storeySlices;
    storeySlices.Begin (*storeys, wantStorySlices && full, wantPlanCut && full, planCutZ, full);

    // From here on the model MUST be released through the gate, on every exit
    // path. One lambda, called from each of them.
    const auto releaseModel = [this, handle] () {
        if (handle->model == nullptr)
            return;
        ModelerAPI::Model* doomed = handle->model;
        handle->model = nullptr;
        GS::UniString err;
        if (stopFlag_.load () && evp::MainThreadGate::Get ().Post ([doomed] { delete doomed; }, err))
            return; // ⚠️ stopped: a joining main thread would revoke an Invoke, leaking it (GateWait.hpp)
        // ⚠️ BY VALUE, and it deletes whenever it runs — including LATE, after a
        // timeout. That is the correct outcome: the object is only ever touched
        // by whoever runs this job, on the main thread. If the gate is gone
        // entirely (the add-on is unloading) this leaks one model rather than
        // calling a DevKit destructor from a worker thread, which is the trade
        // this file makes deliberately.
        if (!evp::MainThreadGate::Get ().Invoke ([doomed] { delete doomed; }, SliceTimeoutMs, err))
            ArchVizLog ("extraction: the gate refused the model release; it will be freed when "
                        "the job dispatches, or leaked if it never does (" +
                        std::string (err.ToCStr ().Get ()) + ")");
    };
    if (abandon) {
        releaseModel ();
        return false;
    }

    if (handle->count <= 0) {
        // ⚠️ AN EMPTY MODEL IS NOT AN ERROR AND MUST NOT LOOK LIKE ONE. It is
        // what a 3D view with everything filtered out, or an unbuilt 3D, returns
        // — and it is what made a live-sync run report success while arming
        // nothing and never updating. Said plainly here so the probe can stop
        // instead of waiting for edits that can never be reported.
        if (auto empty = assembly.Finish (modelwatch::CaptureStamp () == handle->captureStamp))
            MeshStore::Get ().PublishShared (empty);
        releaseModel ();
        fail ("the 3D model is EMPTY (0 elements). Nothing can be extracted or "
              "watched. Open the 3D window and check the building is visible "
              "there - layers, storey filters and the 3D-only-selected setting "
              "all empty it.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.total = uint32_t (handle->count > 0 ? handle->count : 0);
        progress_.materials = (*materials != nullptr) ? uint32_t ((*materials)->Size ()) : 0;
        progress_.phase = partial ? "re-extracting" : "extracting";
    }

    // ---- the batch opens ----------------------------------------------------
    // ⚠️ ORDER: BeginBatch, then the table, then the sun, then geometry. An
    // upsert that reaches the consumer before its table is drawn against the
    // PREVIOUS pool (see the header).
    //
    // ⚠️ THE TABLE IS RE-SENT ON A PARTIAL PASS TOO, and that is not waste. The
    // model pool is RENUMBERED whenever Archicad rebuilds its 3D — which an edit
    // does — so the indices in the re-extracted element refer to the NEW pool.
    // Keeping the old table would repaint one changed wall in another surface's
    // colour and leave the rest of the building right, which is the hardest
    // possible version of this bug to notice.
    // ⚠️ THE TABLE THAT GOES OUT FIRST CARRIES THE *PREVIOUS* PASS'S SUBSTANCES.
    // On a partial pass that is the whole answer (nothing re-derives it). On a
    // full pass it is a placeholder that keeps the model from flashing back to
    // undifferentiated matte for the length of the walk, and it is replaced by
    // the freshly joined table at the end. See substanceMemory_ for why the
    // carry-forward is checked against the surface name rather than trusted.
    MaterialTable surfaces; // with its textures (#252); images are shared, not copied
    if (*materials != nullptr) {
        surfaces = **materials;
        ApplySubstanceMemory (substanceMemory_, **materials);
    }

    // #14: what the slices may reuse from the scene cache, remapped to this pool; null otherwise.
    const auto sliceCache = full && !partial ? sceneCache.Prepare (handle->materialIdentity) : nullptr;
    if (sliceCache != nullptr) {
        sceneCache.NoteMaterials (surfaces);
        if (haveEnv->load ())
            sceneCache.NoteEnvironment (*env);
    }
    SceneCmdQueue::Get ().PushBeginBatch (full, partial ? baseSnapshot->id : 0);
    bool packetsComplete = true;
    if (*materials != nullptr)
        SceneCmdQueue::Get ().PushMaterials (std::move (*materials));
    if (haveEnv->load ())
        SceneCmdQueue::Get ().PushEnvironment (*env);

    // ---- phase 2: walk the model, a few milliseconds at a time --------------
    auto wanted = std::make_shared<const std::set<std::string>> (effectiveFilter);
    std::set<std::string> found;
    std::vector<SurfaceSubstanceObservation> observations;

    int32_t cursor = 1; // 1-BASED
    int32_t changedTo = -1;
    int consecutiveTimeouts = 0;
    bool gaveUp = false;
    int64_t throttledMs = 0;

    while (cursor <= handle->count && !stopFlag_.load ()) {
        if (NowMs () - started >= opt.maxSeconds * 1000) {
            gaveUp = true;
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.phase = "gave up after " + std::to_string (opt.maxSeconds) + "s with " +
                              std::to_string (progress_.pushed) + " of " + std::to_string (progress_.total) +
                              " elements sent";
            break;
        }

        // THE THROTTLE. The render thread applies a bounded number of commands
        // per frame on purpose; without this the extractor would queue the whole
        // project ahead of it and hold the entire snapshot twice in memory.
        // Waiting here rather than extracting is not lost time — it is time
        // Archicad's main thread gets back.
        while (SceneCmdQueue::Get ().PendingBytes () > MaxPendingBytes && !stopFlag_.load ()) {
            std::this_thread::sleep_for (std::chrono::milliseconds (16));
            throttledMs += 16;
        }
        if (stopFlag_.load ())
            break;

        auto st = std::make_shared<SliceState> ();
        st->next.store (cursor);

        ModelerAPI::Model* model = handle->model;
        const int32_t count = handle->count;
        const int64_t sliceMs = opt.sliceMs;
        const int64_t sliceStart = NowMs ();
        GS::UniString sliceErr;
        // RE51: observe substances only when there is something to observe them
        // against. `full` gates the whole join; an empty attribute map means the
        // read failed or the project has no classifiable materials, and walking
        // every body to record `Unknown` for all of them would be pure cost.
        const bool observeSubstances = full && !substances->byAttribute.empty ();
        const bool ok = evp::MainThreadGate::Get ().InvokeUnlessStopped (
            [model, count, st, wanted, sliceMs, observeSubstances, substances, handle, sliceCache] {
                st->started.store (true);
                if (modelwatch::CaptureStamp () != handle->captureStamp) {
                    st->changedTo.store (ModelElementCount (*model));
                    st->completed.store (true);
                    return;
                }
                extractionslice::Run (*model, count, *st, *wanted, sliceMs, observeSubstances, *substances,
                                      sliceCache.get ());
            },
            SliceTimeoutMs, stopFlag_, sliceErr); // unless stopped, as the acquire
        const int64_t sliceElapsed = NowMs () - sliceStart;

        // ⚠️ WAIT FOR A TIMED-OUT SLICE THAT STARTED, NEVER RETRY IT - see the
        // header. One that never started was revoked: post it again
        // (extractionslice::JudgeTimedOutSlice).
        if (!ok && !st->completed.load ()) {
            const int64_t timedOutAt = NowMs ();
            const int64_t deadlineMs = static_cast<int64_t> (SliceTimeoutMs) * 8;
            auto verdict = extractionslice::TimedOutSlice::Wait;
            while (!stopFlag_.load ()) {
                verdict = extractionslice::JudgeTimedOutSlice (st->started.load (), st->completed.load (),
                                                               NowMs () - timedOutAt, deadlineMs);
                if (verdict != extractionslice::TimedOutSlice::Wait)
                    break;
                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    progress_.phase = "extracting - one element is taking longer than a slice";
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (opt.gapMs));
            }
            if (verdict == extractionslice::TimedOutSlice::Retry) {
                ArchVizLog ("pipeline: stage=geometry-slice-retry first=" + std::to_string (cursor) +
                            " gateRoundTripMs=" + std::to_string (NowMs () - sliceStart) +
                            " reason=revoked-before-start");
                std::this_thread::sleep_for (std::chrono::milliseconds (opt.gapMs));
                continue;
            }
            if (st->completed.load ()) {
                std::lock_guard<std::mutex> lock (mutex_);
                progress_.phase = partial ? "re-extracting" : "extracting";
            }
        }

        if (!st->completed.load ()) {
            // ABANDON IT — never harvest a buffer another thread may be writing.
            ++consecutiveTimeouts;
            if (consecutiveTimeouts >= 5) {
                std::lock_guard<std::mutex> lock (mutex_);
                progress_.phase = "element " + std::to_string (cursor) + " of " + std::to_string (handle->count) +
                                  " never finished tessellating. Archicad may "
                                  "be regenerating the 3D model - run Main-Thread Gate Watch to tell those apart";
                break;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (opt.gapMs * 4));
            continue;
        }
        consecutiveTimeouts = 0;
        if ((changedTo = st->changedTo.load ()) >= 0)
            break;

        const int32_t advanced = st->next.load ();
        ArchVizLog ("pipeline: stage=geometry-slice first=" + std::to_string (cursor) +
                    " next=" + std::to_string (advanced) + " elements=" + std::to_string (st->meshes.size ()) +
                    " hostHoldMs=" + std::to_string (st->holdMs.load ()) +
                    " gateRoundTripMs=" + std::to_string (NowMs () - sliceStart));

        // ---- the worker's own half: convert, group, hand over ---------------
        // Off the main thread, while Archicad has it back.
        uint32_t pushed = 0;
        uint64_t triangles = 0;
        for (auto& packet : st->meshes) {
            const Mesh& mesh = packet.mesh;
            if (extractedGuids != nullptr)
                extractedGuids->push_back (mesh.guid);
            storeySlices.Cut (mesh); // no-op unless slices were asked for

            std::unique_ptr<ElementUpload> up = MakeElementPacket (packet);
            assembly.Add (std::move (packet.mesh)); // retained doubles, no second host extraction
            if (up == nullptr) {
                packetsComplete = false;
                continue;
            }
            triangles += up->indices.size () / 3;
            SceneCmdQueue::Get ().PushUpsert (std::move (up));
            ++pushed;
        }
        if (sliceCache != nullptr) // #14: the elements the cache vouched for, sent as captured ones are
            pushed += sceneCache.HarvestSlice (*st, assembly, storeySlices, observations, extractedGuids, triangles,
                                               packetsComplete);
        for (const std::string& guid : st->matched)
            found.insert (guid);
        if (!st->observations.empty ())
            observations.insert (observations.end (), st->observations.begin (), st->observations.end ());

        {
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.extracted += uint32_t (st->meshes.size () + st->reused.size ());
            progress_.empty += st->empty.load ();
            progress_.MergeEmptyKinds (st->emptyByType, st->emptyReasons);
            progress_.pushed += pushed;
            progress_.triangles += triangles;
            ++progress_.slices;
            progress_.longestHoldMs = std::max (progress_.longestHoldMs, st->holdMs.load ());
            progress_.longestRoundTripMs = std::max (progress_.longestRoundTripMs, sliceElapsed);
            progress_.elapsedMs = NowMs () - started;
            progress_.throttledMs = throttledMs;
        }

        if (advanced <= cursor) // no forward progress: refuse to spin forever
            break;
        cursor = advanced;

        // THE YIELD, and the reason this is a background process rather than a
        // slow foreground one: between slices the main thread is entirely the
        // user's. Longer gap = less interference, more latency, and the
        // requirement prefers latency.
        std::this_thread::sleep_for (std::chrono::milliseconds (opt.gapMs));
    }

    // ---- what the model no longer contains ---------------------------------
    // ⚠️ THIS IS HOW A DELETE REACHES THE SCENE, and it needs no event-id
    // interpretation at all: the element was named as changed, the model does
    // not have it, therefore it is gone from the 3D. That also covers the cases
    // an event id would get WRONG — an element moved onto a hidden layer, or off
    // this storey, is equally "not in the model" and equally must disappear.
    uint32_t removed = 0;
    // ⚠️ NOT FROM A WALK THE MODEL CHANGED UNDER: the GUIDs it never reached are not gone.
    if (partial && changedTo < 0) {
        for (const std::string& guid : effectiveFilter) {
            if (found.count (guid) == 0) {
                SceneCmdQueue::Get ().PushRemove (guid);
                ++removed;
            }
        }
    }

    // ---- RE51: the substance join, and the table it produces ---------------
    //
    // ⚠️ A SECOND MATERIAL TABLE ON THE SAME BATCH, DELIBERATELY. The first went
    // out before the geometry because a range that arrives before its table is
    // drawn against the previous pool (SceneCmdQueue.hpp). This one carries the
    // SAME pool with the substance field filled in, which no index depends on --
    // so it is safe here, and here is the only place it CAN go: the vote is not
    // complete until every element has been walked.
    //
    // ⚠️ AND ONLY WHEN THE PASS FINISHED. A pass that was stopped or gave up has
    // seen part of the model, and a partial vote is not a weaker answer -- it is
    // a different one, which would change a surface's substance according to how
    // far a cancelled refresh happened to get.
    if (full && !gaveUp && !observations.empty () && cursor > handle->count) {
        int named = 0;
        std::unique_ptr<MaterialTable> joined = BuildSubstanceTable (surfaces, observations, substanceMemory_, named);
        if (sliceCache != nullptr)
            sceneCache.NoteMaterials (*joined);
        SceneCmdQueue::Get ().PushMaterials (std::move (joined));

        extractionreport::SubstanceJoin (named, surfaces.Size (), substances->classified, substances->total,
                                         observations.size (), substances->error);
    }

    // ---- the storey slices --------------------------------------------------
    // ⚠️ ONLY ON A PASS THAT FINISHED -- the substance vote's rule, sharper; see
    // StorySliceAccumulator::FinishAndPush.
    const bool complete = cursor > handle->count && !gaveUp && !stopFlag_.load () && changedTo < 0 &&
                          modelwatch::CaptureStamp () == handle->captureStamp;
    if (storeySlices.Active () && complete)
        storeySlices.FinishAndPush ();

    // EndBatch is required even when extraction stops partway.
    releaseModel ();
    const auto snapshot = assembly.Finish (complete);
    SceneCmdQueue::Get ().PushEndBatch (packetsComplete ? snapshot : nullptr);
    lastSceneCapture_ = packetsComplete ? snapshot : nullptr;
    if (sliceCache != nullptr)
        sceneCache.FinishFullPass (packetsComplete ? snapshot : nullptr, complete);
    if (snapshot) {
        MeshStore::Get ().PublishShared (snapshot);
        extractionreport::SharedSnapshot (*snapshot, partial);
    }

    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.done = (cursor > handle->count) && !gaveUp;
        progress_.gaveUp = gaveUp;
        progress_.removed += removed;
        progress_.elapsedMs = NowMs () - started;
        progress_.lastPassMs = NowMs () - started;
        progress_.throttledMs = throttledMs;
        if (partial)
            ++progress_.partialPasses;
        else
            ++progress_.fullPasses;
        if (changedTo >= 0)
            progress_.phase = "restarting: the 3D model changed under the pass, " + std::to_string (handle->count) +
                              " -> " + std::to_string (changedTo) + " elements";
        else if (progress_.phase == "extracting" || progress_.phase == "re-extracting")
            progress_.phase = progress_.done ? "idle" : "stopped";

        extractionreport::Pass (progress_, partial, partial ? filter.size () : (size_t) progress_.total, removed);
    }
    // ⚠️ AND AGAIN ON THE MODEL AS IT IS NOW -- a full pass full, since nothing says what changed;
    // an update over the same elements, since the content sweep reads what did (ModelContentWatch)
    // -- after it settles, and a bounded number of times so a model that never settles cannot hold
    // the worker.
    if ((changedTo >= 0 || (!complete && !gaveUp)) && !stopFlag_.load () && restarts_ < kMaxRestarts) {
        ++restarts_;
        std::this_thread::sleep_for (std::chrono::milliseconds (kRestartSettleMs));
        if (extractedGuids != nullptr)
            extractedGuids->clear ();
        return RunPass (opt, full, filter, extractedGuids);
    }
    restarts_ = 0;
    RunDeferredUv (opt, complete); // #14: textured elements' coordinates, once a pass is complete
    return true;
}

} // namespace archviz
} // namespace geomsrv
