#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ArchVizLog.hpp"       // ArchVizLog
#include "ArchViz/ExtractionEnvironment.hpp"   // ReadMaterials, ReadEnvironment
#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/MeshGroups.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "Geometry/GeometryExtractor.hpp"
#include "Notify/ChangeTracker.hpp"   // the viewer is its SECOND consumer - own cursor
#include "Python/MainThreadGate.hpp"
#include "Diagnostics/ApiError.hpp"   // DescribeErr - never print a bare GSErrCode

#include <Model.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace geomsrv {
namespace archviz {

namespace {

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

// ⚠️ NOT MainThreadGate::DefaultTimeoutMs (30 s), for BackgroundArm's reason:
// Stop() joins from the MAIN thread, and a worker sitting inside Invoke is
// waiting for that same thread. Nothing moves until the Invoke times out, so
// this timeout IS the worst-case freeze on close. A slice is ~8 ms of work, so
// 1.5 s is already generous.
constexpr int SliceTimeoutMs = 1500;

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

// How long an acquire may take, in LIVE mode, before the loop stops doing them.
//
// ⚠️ THE POINT IS TO FAIL ONCE, NOT EVERY TIME. Editing in a 2D window
// invalidates the 3D model, so every re-extraction regenerates it — each one
// another multi-second stall, on every edit, for as long as the viewer is
// watching. One slow pass is a cost; the same cost repeating forever is the
// freeze the user reported. Past this, live sync pauses itself and says so.
constexpr int64_t LiveAcquireBudgetMs = 3000;

// Stop queueing when this much is waiting for the GPU. Peak memory is otherwise
// roughly the whole snapshot twice (SceneCmdQueue.hpp's ⚠️ on being unbounded),
// and on a large project the extractor easily outruns a 32-command-per-frame
// consumer.
//
// 96 MiB is about 1.5 M vertices in flight — far more than a frame's worth,
// small enough that the viewer's memory does not track the project's size.
constexpr size_t MaxPendingBytes = 96u * 1024u * 1024u;

// What one extraction slice reports back. ATOMICS and shared BY VALUE, because
// a timed-out Invoke may still run the job LATER, after this loop iteration has
// moved on — the gate's contract says so explicitly. `meshes` is plain (not
// atomic) and is read ONLY when `completed` is true AND the Invoke returned ok;
// a slice that timed out is ABANDONED whole, never harvested, and the shared_ptr
// keeps it alive for the late writer.
struct SliceState {
    std::atomic<int32_t>  next      { 1 };   // 1-BASED: ModelerAPI indices are
    std::atomic<uint32_t> empty     { 0 };
    std::atomic<int64_t>  holdMs    { 0 };
    std::atomic<bool>     completed { false };
    std::vector<Mesh>     meshes;
    // PARTIAL PASSES ONLY: which of the wanted GUIDs this slice actually saw in
    // the model. What is NOT seen by the end of the pass is what was deleted (or
    // hidden, or moved off the storey), and that is how a removal is derived
    // without interpreting an event id.
    std::vector<std::string> matched;
};

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
    int32_t            count = 0;

    // ⚠️ SET WHEN THE ACQUIRE TIMES OUT, AND THE LAMBDA MUST CHECK IT. A gate
    // Invoke that reports a timeout may STILL RUN LATER — the contract says so
    // — and by then the pass that asked for the model has given up. Without this
    // the late job would hand a freshly generated model to nobody and leak it,
    // once per abandoned refresh. Checked on the MAIN thread, inside the job, so
    // the delete happens where it is legal.
    std::atomic<bool>  abandoned { false };
};

// One extracted Mesh -> one ElementUpload, ready for the GPU.
//
// ⚠️ THIS RUNS ON THE WORKER THREAD, BETWEEN SLICES, and that is the whole
// division of labour (see the header). Nothing here is ACAPI; all of it is the
// per-vertex and per-triangle work that would otherwise be charged to Archicad's
// main thread for no reason.
std::unique_ptr<ElementUpload> MakeUpload (const Mesh& mesh)
{
    if (mesh.triangles.empty () || mesh.vertices.empty ())
        return nullptr;

    auto up = std::make_unique<ElementUpload> ();
    up->guid = mesh.guid;

    // double -> float. See ElementUpload's ⚠️ on georeferenced projects: this is
    // where the ~10 cm quantisation would enter, and `bounds` is carried so a
    // later rebase can fix it without re-extracting (PLAT-BGFX-P6-PRECISION).
    up->vertices.resize (mesh.vertices.size ());
    for (size_t i = 0; i < mesh.vertices.size (); ++i)
        up->vertices[i] = static_cast<float> (mesh.vertices[i]);

    up->normals = mesh.normals;   // already float, already per-corner

    // ⚠️ THE ONE ALGORITHM THAT FAILS SILENTLY (plan §6.3). Skipping it renders
    // every surface of an element in the first material's colour.
    BuildMaterialGroups (mesh.triangles, mesh.triMaterial, up->indices, up->ranges);
    if (up->indices.empty ())
        return nullptr;

    if (mesh.bounds.Valid ()) {
        for (int k = 0; k < 3; ++k) {
            up->boundsMin[k] = static_cast<float> (mesh.bounds.mn[k]);
            up->boundsMax[k] = static_cast<float> (mesh.bounds.mx[k]);
        }
    } else {
        // An element whose AABB never got expanded still has vertices; deriving
        // the box here keeps it out of the world bounds' "zoom to fit" as a
        // point at infinity, which would frame the camera on nothing.
        for (int k = 0; k < 3; ++k) {
            up->boundsMin[k] =  1e30f;
            up->boundsMax[k] = -1e30f;
        }
        for (size_t i = 0; i + 2 < up->vertices.size (); i += 3) {
            for (int k = 0; k < 3; ++k) {
                up->boundsMin[k] = std::min (up->boundsMin[k], up->vertices[i + k]);
                up->boundsMax[k] = std::max (up->boundsMax[k], up->vertices[i + k]);
            }
        }
    }
    return up;
}

}   // namespace

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
    opt.mode       = Mode::Once;
    opt.full       = full;
    opt.sliceMs    = sliceMs;
    opt.gapMs      = gapMs;
    opt.maxSeconds = maxSeconds;
    StartWith (opt);
}

void ExtractionWorker::StartLive (int64_t settleMs, int64_t pollMs,
                                  int64_t sliceMs, int64_t gapMs, bool armObservers)
{
    Options opt;
    opt.mode     = Mode::Live;
    opt.armObservers = armObservers;
    opt.full     = true;        // live sync begins from a known-complete scene
    opt.sliceMs  = sliceMs;
    opt.gapMs    = gapMs;
    opt.settleMs = settleMs;
    opt.pollMs   = pollMs;
    // No wall clock: a watch loop is meant to run for as long as the viewer is
    // open. The per-PASS budget below is what bounds any single re-extraction.
    opt.maxSeconds = 24 * 60 * 60;
    StartWith (opt);
}

void ExtractionWorker::StartWith (const Options& opt)
{
    Stop ();                    // a restart supersedes the pass in flight

    stopFlag_.store (false);
    running_.store (true);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_ = Progress ();
        progress_.running = true;
        progress_.live    = (opt.mode == Mode::Live);
        progress_.phase   = "starting";
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
        worker_.join ();        // bounded by SliceTimeoutMs — see the header
    running_.store (false);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.running = false;
        progress_.live    = false;
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
    const bool ok = RunPass (opt, opt.full, std::set<std::string> (), &extracted);

    if (opt.mode == Mode::Live && ok && !stopFlag_.load ()) {
        // ⚠⚠ NO OBSERVERS, NO WATCH LOOP — AND THE SECOND HALF IS AS IMPORTANT AS
        // THE FIRST (PLAT-RE68). Arming writes to the project database (see
        // StartLive), so it is off by default; but RunLiveLoop's idle branch
        // treats "not watching" as a DROPPED watch and answers it with a FULL
        // re-extraction, so leaving the loop running with nothing armed would
        // re-extract the whole model forever. One pass, then stop, and say so.
        if (!opt.armObservers) {
            ArchVizLog ("extraction: live sync is running as a SINGLE PASS — per-element "
                        "change observers are disabled because attaching them writes a link "
                        "into the project database and makes Archicad autosave continuously "
                        "(PLAT-RE68). The scene will not follow edits; reopen the viewer to "
                        "refresh it.");
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.phase = "one pass (live refresh disabled: see archviz.log)";
        } else {
            ArmObservers (extracted, opt);
            RunLiveLoop (opt);
        }
    }

    running_.store (false);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.running = false;
        progress_.live    = false;
    }
}

// ---------------------------------------------------------------------------
// ONE PASS. `filter` empty = the whole model; otherwise only those GUIDs, and
// anything named in the filter that the model does NOT contain is pushed as a
// REMOVAL — which is how a deleted element leaves the scene without anything
// having to interpret Archicad's event ids.
// ---------------------------------------------------------------------------
bool ExtractionWorker::RunPass (const Options& opt, bool full,
                                const std::set<std::string>& filter,
                                std::vector<std::string>* extractedGuids)
{
    const int64_t started  = NowMs ();
    const bool    partial  = !filter.empty ();

    const auto fail = [this, started] (const std::string& why) {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.phase     = why;
        progress_.elapsedMs = NowMs () - started;
        ArchVizLog ("extraction: " + why);
    };

    {
        // Per-pass counters reset; the cumulative ones (fullPasses, removed,
        // armed) deliberately do not — over a long live session those are the
        // history, and resetting them would make the viewer look like it had
        // just started every time a wall moved.
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.total = progress_.extracted = progress_.empty = progress_.pushed = 0;
        progress_.triangles = 0;
        progress_.slices    = 0;
        progress_.longestHoldMs = progress_.longestRoundTripMs = 0;
        progress_.acquireMs = progress_.throttledMs = 0;
        progress_.done = progress_.gaveUp = false;
        progress_.phase = "acquiring";
    }

    // ---- phase 1: acquire the model, the surface pool and the sun -----------
    // ONE slice, because AcquireCurrentModel is one uninterruptible call. The
    // pool and the sun ride along: both are small, both are ModelerAPI/ACAPI,
    // and a second gate hop for them would cost a round trip to save nothing.
    auto handle    = std::make_shared<ModelHandle> ();
    auto materials = std::make_shared<std::unique_ptr<MaterialTable>> ();
    auto env       = std::make_shared<EnvironmentUpload> ();
    auto haveEnv   = std::make_shared<std::atomic<bool>> (false);

    const int64_t acquireStart = NowMs ();
    GS::UniString gateErr;
    const bool acquired = evp::MainThreadGate::Get ().Invoke (
        [handle, materials, env, haveEnv] {
            auto model = std::make_unique<ModelerAPI::Model> ();
            if (!AcquireCurrentModel (*model))
                return;                       // handle->model stays null: "no 3D model"

            // ⚠️ BEFORE ANYTHING ELSE IS DONE WITH IT. If the pass gave up while
            // Archicad was generating, this job is now the model's only owner
            // and the only thread allowed to destroy it. Letting the unique_ptr
            // go out of scope here does exactly that, on the main thread.
            if (handle->abandoned.load ())
                return;

            handle->count  = ModelElementCount (*model);
            *materials     = ReadMaterials (*model);
            haveEnv->store (ReadEnvironment (*env));
            handle->model  = model.release (); // ⚠️ ownership moves to the pass
        },
        AcquireTimeoutMs, gateErr);
    const int64_t acquireMs = NowMs () - acquireStart;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.acquireMs          = acquireMs;
        progress_.longestHoldMs      = std::max (progress_.longestHoldMs, acquireMs);
        progress_.longestRoundTripMs = std::max (progress_.longestRoundTripMs, acquireMs);
    }

    if (!acquired) {
        // ⚠️ ABANDON IT PROPERLY. The job may still be running (Archicad is
        // generating the 3D model right now) and may finish after this line; the
        // flag is what tells it to destroy what it built instead of leaking it.
        handle->abandoned.store (true);
        fail ("gave up waiting " + std::to_string (AcquireTimeoutMs / 1000) +
              "s for the 3D model. Archicad is almost certainly GENERATING it - "
              "that happens when the 3D window has never been opened, or when an "
              "edit in a 2D window invalidated it. Open the 3D window once, let it "
              "finish drawing, then refresh.");
        return false;
    }
    if (handle->model == nullptr) {
        fail ("this project has no 3D model to show - open the 3D window once, then refresh");
        return false;
    }

    // From here on the model MUST be released through the gate, on every exit
    // path. One lambda, called from each of them.
    const auto releaseModel = [handle] () {
        if (handle->model == nullptr)
            return;
        ModelerAPI::Model* doomed = handle->model;
        handle->model = nullptr;
        GS::UniString err;
        // ⚠️ BY VALUE, and it deletes whenever it runs — including LATE, after a
        // timeout. That is the correct outcome: the object is only ever touched
        // by whoever runs this job, on the main thread. If the gate is gone
        // entirely (the add-on is unloading) this leaks one model rather than
        // calling a DevKit destructor from a worker thread, which is the trade
        // this file makes deliberately.
        if (!evp::MainThreadGate::Get ().Invoke ([doomed] { delete doomed; },
                                                 SliceTimeoutMs, err))
            ArchVizLog ("extraction: the gate refused the model release; it will be freed when "
                        "the job dispatches, or leaked if it never does (" +
                        std::string (err.ToCStr ().Get ()) + ")");
    };

    if (handle->count <= 0) {
        // ⚠️ AN EMPTY MODEL IS NOT AN ERROR AND MUST NOT LOOK LIKE ONE. It is
        // what a 3D view with everything filtered out, or an unbuilt 3D, returns
        // — and it is what made a live-sync run report success while arming
        // nothing and never updating. Said plainly here so the probe can stop
        // instead of waiting for edits that can never be reported.
        releaseModel ();
        fail ("the 3D model is EMPTY (0 elements). Nothing can be extracted or "
              "watched. Open the 3D window and check the building is visible "
              "there - layers, storey filters and the 3D-only-selected setting "
              "all empty it.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.total     = uint32_t (handle->count > 0 ? handle->count : 0);
        progress_.materials = (*materials != nullptr) ? uint32_t ((*materials)->Size ()) : 0;
        progress_.phase     = partial ? "re-extracting" : "extracting";
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
    SceneCmdQueue::Get ().PushBeginBatch (full);
    if (*materials != nullptr)
        SceneCmdQueue::Get ().PushMaterials (std::move (*materials));
    if (haveEnv->load ())
        SceneCmdQueue::Get ().PushEnvironment (*env);

    // ---- phase 2: walk the model, a few milliseconds at a time --------------
    auto wanted = std::make_shared<const std::set<std::string>> (filter);
    std::set<std::string> found;

    int32_t  cursor              = 1;        // 1-BASED
    int      consecutiveTimeouts = 0;
    bool     gaveUp              = false;
    int64_t  throttledMs         = 0;

    while (cursor <= handle->count && !stopFlag_.load ()) {
        if (NowMs () - started >= opt.maxSeconds * 1000) {
            gaveUp = true;
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.phase = "gave up after " + std::to_string (opt.maxSeconds) + "s with " +
                              std::to_string (progress_.pushed) + " of " +
                              std::to_string (progress_.total) + " elements sent";
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
        const int32_t      count = handle->count;
        const int64_t   sliceMs  = opt.sliceMs;
        const int64_t sliceStart = NowMs ();
        GS::UniString sliceErr;
        const bool ok = evp::MainThreadGate::Get ().Invoke (
            [model, count, st, wanted, sliceMs] {
                // The budget is checked INSIDE the slice, on the main thread, so
                // the bound is on how long Archicad is actually held — not on
                // how many elements we guessed would fit.
                //
                // ⚠️ AT LEAST ONE ELEMENT PER SLICE, ALWAYS. A single element
                // can take longer than the whole budget (a curtain wall, a
                // stair), and a loop that checked the clock FIRST would extract
                // nothing, report no progress, and spin forever on that element.
                const int64_t begin = NowMs ();
                int32_t i = st->next.load ();
                while (i <= count) {
                    if (wanted->empty ()) {
                        Mesh mesh;
                        if (ExtractElementAt (*model, i, mesh))
                            st->meshes.push_back (std::move (mesh));
                        else
                            st->empty.fetch_add (1);   // a 2D-only element, ordinary
                    } else {
                        // ⚠️ THE GUID FIRST, THE GEOMETRY ONLY IF IT MATCHES.
                        // Tessellating an element to discover it was not the one
                        // that changed is the entire cost of the pass, spent on
                        // nothing — which would make a partial refresh as
                        // expensive as a full one and leave it no reason to
                        // exist.
                        const std::string guid = ElementGuidAt (*model, i);
                        if (!guid.empty () && wanted->count (guid) > 0) {
                            Mesh mesh;
                            if (ExtractElementAt (*model, i, mesh))
                                st->meshes.push_back (std::move (mesh));
                            else
                                st->empty.fetch_add (1);
                            st->matched.push_back (guid);
                        }
                    }
                    ++i;
                    if (NowMs () - begin >= sliceMs)
                        break;
                }
                st->next.store (i);
                st->holdMs.store (NowMs () - begin);
                st->completed.store (true);
            },
            SliceTimeoutMs, sliceErr);
        const int64_t sliceElapsed = NowMs () - sliceStart;

        if (!ok || !st->completed.load ()) {
            // ABANDON THE WHOLE SLICE — do not harvest its meshes. The job may
            // still be running or may run later, and the shared_ptr keeps its
            // state alive for that writer. Retrying the same cursor costs one
            // re-extraction; reading a buffer another thread is writing costs a
            // crash that only happens when Archicad is busy.
            ++consecutiveTimeouts;
            if (consecutiveTimeouts >= 5) {
                std::lock_guard<std::mutex> lock (mutex_);
                progress_.phase = "the main-thread gate stopped dispatching; extraction paused";
                break;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (opt.gapMs * 4));
            continue;
        }
        consecutiveTimeouts = 0;

        const int32_t advanced = st->next.load ();

        // ---- the worker's own half: convert, group, hand over ---------------
        // Off the main thread, while Archicad has it back.
        uint32_t pushed    = 0;
        uint64_t triangles = 0;
        for (const Mesh& mesh : st->meshes) {
            if (extractedGuids != nullptr)
                extractedGuids->push_back (mesh.guid);
            std::unique_ptr<ElementUpload> up = MakeUpload (mesh);
            if (up == nullptr)
                continue;
            triangles += up->indices.size () / 3;
            SceneCmdQueue::Get ().PushUpsert (std::move (up));
            ++pushed;
        }
        for (const std::string& guid : st->matched)
            found.insert (guid);

        {
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.extracted += uint32_t (st->meshes.size ());
            progress_.empty     += st->empty.load ();
            progress_.pushed    += pushed;
            progress_.triangles += triangles;
            ++progress_.slices;
            progress_.longestHoldMs      = std::max (progress_.longestHoldMs, st->holdMs.load ());
            progress_.longestRoundTripMs = std::max (progress_.longestRoundTripMs, sliceElapsed);
            progress_.elapsedMs          = NowMs () - started;
            progress_.throttledMs        = throttledMs;
        }

        if (advanced <= cursor)     // no forward progress: refuse to spin forever
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
    if (partial) {
        for (const std::string& guid : filter) {
            if (found.count (guid) == 0) {
                SceneCmdQueue::Get ().PushRemove (guid);
                ++removed;
            }
        }
    }

    // ---- close the batch ----------------------------------------------------
    // EndBatch even on a stop or a give-up. The consumer clears at BEGIN, not at
    // end (SceneCache.cpp), so this cannot delete anything — it only says "no
    // more is coming", which is what makes a partial load visibly finished
    // rather than apparently still streaming.
    SceneCmdQueue::Get ().PushEndBatch ();

    releaseModel ();

    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.done        = (cursor > handle->count) && !gaveUp;
        progress_.gaveUp      = gaveUp;
        progress_.removed    += removed;
        progress_.elapsedMs   = NowMs () - started;
        progress_.lastPassMs  = NowMs () - started;
        progress_.throttledMs = throttledMs;
        if (partial)
            ++progress_.partialPasses;
        else
            ++progress_.fullPasses;
        if (progress_.phase == "extracting" || progress_.phase == "re-extracting")
            progress_.phase = progress_.done ? "idle" : "stopped";

        ArchVizLog ("extraction: " + progress_.phase +
                    (partial ? " (partial)" : " (full)") +
                    " - " + std::to_string (progress_.pushed) + "/" +
                    std::to_string (partial ? filter.size () : (size_t) progress_.total) +
                    " elements, " + std::to_string (removed) + " removed, " +
                    std::to_string (progress_.triangles) + " triangles, " +
                    std::to_string (progress_.materials) + " surfaces, " +
                    std::to_string (progress_.slices) + " slices, longest hold " +
                    std::to_string (progress_.longestHoldMs) + " ms, acquire " +
                    std::to_string (progress_.acquireMs) + " ms, total " +
                    std::to_string (progress_.elapsedMs) + " ms");
    }
    return true;
}

// ---------------------------------------------------------------------------
// ARMING. Attach a change observer to every element the pass actually extracted.
//
// ⚠️ `scope=visible` BY CONSTRUCTION — the list is what the 3D SIGHT yielded, so
// hidden layers, other storeys and filtered-out elements are never armed. They
// cannot change a pixel, and the measured alternative (attach to every element
// in the database) managed ~9 attaches/second, i.e. ~22 minutes on a
// 12k-element project. See EvP.WatchModel's own warning; this is that decision,
// applied by construction rather than by a flag.
//
// ⚠️ SLICED, like everything else here. ACAPI_Element_AttachObserver is main
// thread, and a visible set can still be thousands of elements.
// ---------------------------------------------------------------------------
void ExtractionWorker::ArmObservers (const std::vector<std::string>& guids, const Options& opt)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.phase = "arming";
    }

    // ⚠️⚠️ INSTALL THE HANDLERS FIRST, AND THIS IS THE BUG THE 2026-08-07 RUN
    // ACTUALLY HAD. `ACAPI_Element_AttachObserver` LINKS an element to this
    // add-on; it does NOT make Archicad call anything. Delivery needs
    // `ACAPI_Element_InstallElementObserver` (and `CatchNewElement` for
    // creations), which until now was called from ONE place: EvP.WatchModel,
    // the Python path. Live sync attached observers to a few hundred elements,
    // reported `armed: 388`, went to "watching", and could never hear a single
    // notification — a watch that is armed, healthy-looking and DEAF.
    //
    // Nothing in the reported symptoms could distinguish that from "the model is
    // not changing", which is why the first two diagnoses were wrong. It is
    // idempotent, so calling it on every arm costs one main-thread hop.
    auto installErr = std::make_shared<std::atomic<GSErrCode>> (NoError);
    {
        GS::UniString err;
        if (!evp::MainThreadGate::Get ().Invoke (
                [installErr] { installErr->store (InstallChangeObserver ()); },
                SliceTimeoutMs, err) ||
            installErr->load () != NoError) {
            ChangeTracker::Get ().SetWatching (false, 0);
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.armed = 0;
            progress_.phase =
                "the change observer could not be INSTALLED, so no edit can ever be "
                "reported however many elements are attached (" +
                std::string (evp::DescribeErr (installErr->load ()).ToCStr ().Get ()) + ")";
            ArchVizLog ("extraction: " + progress_.phase);
            return;
        }
    }

    auto all = std::make_shared<std::vector<std::string>> (guids);
    size_t   cursor   = 0;
    uint32_t attached = 0;
    uint32_t refused  = 0;

    while (cursor < all->size () && !stopFlag_.load ()) {
        auto next        = std::make_shared<std::atomic<size_t>> (cursor);
        auto attachedNow = std::make_shared<std::atomic<uint32_t>> (0);
        auto refusedNow  = std::make_shared<std::atomic<uint32_t>> (0);
        auto completed   = std::make_shared<std::atomic<bool>> (false);
        const int64_t sliceMs = opt.sliceMs;

        GS::UniString err;
        const bool ok = evp::MainThreadGate::Get ().Invoke (
            [all, next, attachedNow, refusedNow, completed, sliceMs] {
                const int64_t begin = NowMs ();
                size_t i = next->load ();
                for (; i < all->size (); ++i) {
                    if (NowMs () - begin >= sliceMs)
                        break;
                    const API_Guid guid = APIGuidFromString ((*all)[i].c_str ());
                    const GSErrCode e = ACAPI_Element_AttachObserver (guid);
                    // APIERR_LINKEXIST = already watched: the normal case on a
                    // re-arm, and not a failure.
                    if (e == NoError || e == APIERR_LINKEXIST)
                        attachedNow->fetch_add (1);
                    else
                        // ⚠️ COUNTED, NOT SWALLOWED. The modeler yields sub-part
                        // GUIDs (stair treads, curtain-wall panels) that the
                        // element database may refuse to observe; "armed 388,
                        // refused 0" and "armed 12, refused 376" describe
                        // completely different watches and used to print the
                        // same way.
                        refusedNow->fetch_add (1);
                }
                next->store (i);
                completed->store (true);
            },
            SliceTimeoutMs, err);

        if (!ok || !completed->load ())
            break;                      // same discipline as an extraction slice

        const size_t advanced = next->load ();
        attached += attachedNow->load ();
        refused  += refusedNow->load ();
        if (advanced <= cursor)
            break;
        cursor = advanced;

        std::this_thread::sleep_for (std::chrono::milliseconds (opt.gapMs));
    }

    // ⚠️ A WATCH OF NOTHING IS THE FAILURE THIS REPORTS. Everything downstream
    // looks identical whether the observer is armed or not — the viewer simply
    // never updates — so the count is the only thing that separates "the model
    // is not changing" from "we are not listening".
    ChangeTracker::Get ().SetWatching (attached > 0, attached);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.armed        = attached;
        progress_.armRefused   = refused;
        progress_.handlersInstalled = true;
        progress_.phase = (attached > 0) ? "watching" : "watching nothing - no elements armed";
    }
    ArchVizLog ("extraction: handlers installed; armed " + std::to_string (attached) +
                " of " + std::to_string (all->size ()) + " extracted elements (" +
                std::to_string (refused) + " refused)");
}

// ---------------------------------------------------------------------------
// THE WATCH LOOP. Plan §6.5, and the phase Part I exists for: move a wall in
// Archicad, watch it move in the viewer.
// ---------------------------------------------------------------------------
void ExtractionWorker::RunLiveLoop (const Options& opt)
{
    ChangeTracker& tracker = ChangeTracker::Get ();

    // ⚠️ OUR OWN CURSOR. The bus (EvP.TakeChanges) has its own; sharing one
    // would mean whichever drained first hid the change from the other. See
    // ChangeTracker's consumer block — this is the second consumer that made
    // per-consumer cursors a prerequisite rather than a nicety.
    const ChangeTracker::ConsumerId me = tracker.RegisterConsumer ("archviz");

    // What the LAST pass spent waiting for the model. Read at the top of the
    // next iteration by the freeze guard below; seeded from the initial full
    // pass, so a live sync started from a 2D window pauses BEFORE its first
    // re-extraction rather than after.
    int64_t lastAcquireMs = 0;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        lastAcquireMs = progress_.acquireMs;
    }

    while (!stopFlag_.load ()) {
        std::this_thread::sleep_for (std::chrono::milliseconds (opt.pollMs));
        if (stopFlag_.load ())
            break;

        std::vector<ChangeTracker::Entry> entries;
        size_t remaining  = 0;
        bool   overflowed = false;
        tracker.TakeDirtyFor (me, 4096, /*peek*/ true, entries, remaining, overflowed);

        {
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.dirtyPending = uint32_t (entries.size () + remaining);
        }

        if (entries.empty ()) {
            // ⚠️ A DROPPED WATCH IS NOT AN IDLE MODEL. IsWatching goes false on a
            // project event, and from that moment nothing will ever be reported
            // again — the viewer would sit showing a stale building and looking
            // perfectly healthy. A full pass rebuilds and re-arms.
            if (!tracker.IsWatching ()) {
                std::vector<std::string> extracted;
                if (RunPass (opt, /*full*/ true, std::set<std::string> (), &extracted) &&
                    !stopFlag_.load ())
                    ArmObservers (extracted, opt);
            }
            continue;
        }

        // ⚠️ LET THE EDIT FINISH. A drag notifies per frame; re-extracting on
        // every notification competes with the user's own edit for the main
        // thread and produces a viewer that lags precisely while it is being
        // watched. IdleMs is the model going quiet, not a timer of ours.
        const int64_t idle = tracker.IdleMs ();
        if (idle >= 0 && idle < opt.settleMs)
            continue;

        // When the change actually happened, so the reported latency is
        // edit -> pixel rather than merely how long our own pass took.
        const int64_t changedAt = NowMs () - (idle > 0 ? idle : 0);

        // ⚠️ OVERFLOW OR AN OPAQUE ENTRY MEANS FULL, and the branch comes BEFORE
        // the guid list. `overflowed` says the dirty set hit its cap and dropped
        // elements — the list is not the whole story. An opaque entry (null
        // guid) is EvP's own write or a project event: identity unknown, and
        // never mappable to a mesh.
        bool wantFull = overflowed;
        for (const ChangeTracker::Entry& e : entries) {
            if (e.eventId == ChangeTracker::OpaqueEventId) {
                wantFull = true;
                break;
            }
        }

        // ⚠️ THE FREEZE GUARD, and it is the lesson of the 2026-08-07 run. If
        // the previous pass had to wait on Archicad REGENERATING its 3D model,
        // the next one will too — editing in a 2D window invalidates the 3D, so
        // "one slow pass" becomes a multi-second stall on every single edit, for
        // as long as the viewer watches. Live sync pauses itself instead, and
        // says which window to be in to make it cheap again. Nothing here can
        // make the regeneration fast; what it can do is stop paying for it
        // repeatedly without telling anyone.
        if (lastAcquireMs > LiveAcquireBudgetMs) {
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.phase =
                "live sync PAUSED: the last refresh spent " +
                std::to_string (lastAcquireMs) + " ms waiting for Archicad to regenerate "
                "its 3D model, and every edit would cost that again. Work with the 3D "
                "window open (it stays generated), then start live sync again.";
            ArchVizLog ("extraction: " + progress_.phase);
            break;
        }

        bool ok = false;
        std::vector<std::string> extracted;
        if (wantFull) {
            ok = RunPass (opt, /*full*/ true, std::set<std::string> (), &extracted);
            if (ok && !stopFlag_.load ())
                ArmObservers (extracted, opt);
        } else {
            // ⚠️ EXPAND TO SUB-PARTS FIRST. Archicad notifies about the PARENT
            // stair / railing / curtain wall; the modeler only ever yields its
            // sub-parts, under different GUIDs. Without this the pass looks up a
            // GUID the model does not contain, finds nothing, removes nothing,
            // and leaves the old stair on screen while reporting success.
            auto expanded = std::make_shared<std::set<std::string>> ();
            auto guids    = std::make_shared<std::vector<API_Guid>> ();
            for (const ChangeTracker::Entry& e : entries)
                guids->push_back (e.guid);

            GS::UniString err;
            const bool gated = evp::MainThreadGate::Get ().Invoke (
                [guids, expanded] {
                    for (const API_Guid& g : *guids)
                        ExpandElementAndParts (g, *expanded);
                },
                SliceTimeoutMs, err);
            if (!gated)
                continue;               // try again next tick; nothing was drained

            ok = RunPass (opt, /*full*/ false, *expanded, nullptr);
        }

        // ⚠️ DRAIN ONLY NOW. TakeDirtyFor advances a cursor and nothing can put
        // an entry back; committing before the pass succeeded would lose the
        // change entirely on a failure — silently, because a failed pass leaves
        // the OLD geometry on screen, looking correct.
        if (ok) {
            std::vector<ChangeTracker::Entry> drained;
            size_t rest = 0;
            bool   of   = false;
            tracker.TakeDirtyFor (me, entries.size (), /*peek*/ false, drained, rest, of);

            std::lock_guard<std::mutex> lock (mutex_);
            progress_.lastSyncMs   = NowMs () - changedAt;
            progress_.dirtyPending = uint32_t (rest);
            lastAcquireMs          = progress_.acquireMs;
        } else {
            // A pass that could not run at all — the acquire timed out, or the
            // model came back empty. Both are already spelled out in `phase`,
            // and both will repeat on the next tick, so the loop stops rather
            // than hammering a project that cannot answer.
            std::lock_guard<std::mutex> lock (mutex_);
            ArchVizLog ("extraction: live sync stopping - " + progress_.phase);
            break;
        }
    }

    // ⚠️ UNREGISTER, UNCONDITIONALLY. A registered consumer that never drains
    // pins the dirty set: nothing can be collected past its cursor, the set
    // climbs to MaxDirty, and the PYTHON watcher starts reporting overflow. A
    // closed viewer must cost the rest of EvP nothing.
    tracker.UnregisterConsumer (me);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.live  = false;
        progress_.phase = "stopped watching";
    }
}

}   // namespace archviz
}   // namespace geomsrv
