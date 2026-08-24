#ifndef EVP_ARCHVIZ_EXTRACTIONTHREAD_HPP
#define EVP_ARCHVIZ_EXTRACTIONTHREAD_HPP

// Phase 6's producer: Archicad's 3D model -> SceneCmdQueue, WITHOUT EVER
// STALLING ARCHICAD.
//
// THE SHAPE IS Notify/BackgroundArm's, deliberately and almost line for line.
// Read that header first — it is the file this one is modelled on, and the
// reasoning there (why a thread and not a self-rescheduling Post; why the join
// is bounded by a short slice timeout; why a timed-out slice's counters are not
// trustworthy) is the same reasoning here and is not repeated in full.
//
// WHY IT CANNOT JUST CALL ExtractAllElements. That function walks the whole
// model in ONE uninterruptible ACAPI call. Inside MainThreadGate::Invoke that is
// the main thread held for as long as the walk takes — seconds to minutes on a
// real project — which is precisely what plan §0 forbids and what E25 already
// paid for once by hanging the application. So the walk is opened up:
// `ExtractElementAt` (Geometry/GeometryExtractor.hpp) does ONE element, and this
// thread submits as many as fit in a few milliseconds, then gets off.
//
//   worker: [Invoke 8ms of elements] convert+group [Invoke 8ms] convert+group ..
//   main:   ...user edits...........^^^^^^^^^^^^^^..user edits..^^^^^^^^^^^^^..
//
// ⚠️ THE DIVISION OF LABOUR IS THE POINT, and it is not "do everything in the
// slice". Inside the gate the worker does ONLY what ACAPI requires: read the
// element's triangles. The double->float conversion, the material grouping and
// the heap allocation of the upload all happen on THIS thread, between slices,
// while Archicad has its main thread back. Doing them inside the slice would
// roughly double what the user feels for no benefit whatsoever.
//
// ⚠️ THE MODEL IS ACQUIRED ONCE AND HELD ACROSS SLICES. `ModelerAPI::Model` is a
// reference to the modeler's own model; re-acquiring per slice would cost a
// sight switch each time AND could produce a scene stitched from two different
// states of the project. Held, every element in one pass comes from one model.
// ⚠️ AND IT MUST BE DESTROYED ON THE MAIN THREAD. It is a DevKit object with a
// DevKit destructor; letting the worker's stack unwind it is an ACAPI call from
// a worker thread through the back door. The pass therefore releases it inside a
// final gate slice — see `Run`'s teardown, which is the one thing in this file
// with no analogue in BackgroundArm.
//
// ⚠️ ORDER ON THE QUEUE IS LOAD-BEARING: BeginBatch(full) → SetMaterials →
// SetEnvironment → upserts → EndBatch. The pool is renumbered on every model
// rebuild, so an upsert that reaches the consumer before its table is drawn
// against the PREVIOUS pool — the right building in another material's colours,
// which reads as a styling choice rather than a bug.
//
// ⚠️ STOP BEFORE THE ADD-ON UNLOADS, exactly as with ArmWorker. Stop() joins;
// FreeData calls it. A thread still submitting gate jobs into an unloading
// add-on is the one fatal outcome here.

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "ArchViz/SubstanceJoin.hpp"

#include <map>
#include <utility>

namespace geomsrv {
namespace archviz {

// ---------------------------------------------------------------------------
// PHASE 7 — LIVE SYNC, and why it is the same worker rather than a second one.
//
// A live pass and a one-shot pass do the same work; only the trigger differs. Two
// threads doing it would be able to run AT ONCE — two walks of the model, two
// interleaved batches on a queue whose ordering is load-bearing, and a
// BeginBatch(full) from one clearing the other's uploads. One thread with a mode
// makes that unrepresentable.
//
// THE LOOP (plan §6.5):
//
//   if !watching            -> a project event dropped the watch: full pass, re-arm
//   if nothing dirty        -> sleep
//   if IdleMs < settleMs    -> sleep; the user is still dragging
//   peek the dirty entries
//   overflowed or opaque    -> FULL pass
//   otherwise               -> partial pass over those guids
//   drain (peek=false) ONLY after the pass succeeded
//
// ⚠️ PEEK, EXTRACT, THEN DRAIN. `TakeDirty` advances a cursor and nothing can put
// it back; draining first and then failing loses the change silently.
//
// ⚠️ THE VIEWER IS A SECOND ChangeTracker CONSUMER, and it may not use the bus's
// cursor. It registers its own (`ChangeTracker::RegisterConsumer`) and
// UNREGISTERS when it stops — a registered consumer that never drains pins the
// dirty set and eventually starves the Python watcher.
//
// ⚠️ AN OPAQUE ENTRY (null guid, `OpaqueEventId`) MEANS FULL. It is EvP's own
// write or a project event: identity unknown, and never mappable to a mesh.
//
// ⚠️ SETTLE IS NOT A PERFORMANCE TUNING KNOB. A drag notifies per frame; without
// it the viewer re-extracts continuously while the user is still moving the wall,
// which competes with the edit itself for the main thread.
// ---------------------------------------------------------------------------

class ExtractionWorker final {
public:
    static ExtractionWorker& Get ();

    enum class Mode : uint8_t {
        Once,   // one pass, then the thread exits
        Live,   // one pass, then arm, then watch and re-extract what changes
    };

    struct Progress {
        bool     running  = false;
        bool     done     = false;   // completed a full pass
        bool     gaveUp   = false;   // hit maxSeconds with elements left
        uint32_t total    = 0;       // elements the model reported
        uint32_t extracted = 0;      // elements that yielded geometry
        uint32_t empty    = 0;       // elements with nothing drawable (ordinary)
        uint32_t pushed   = 0;       // uploads handed to the queue
        uint32_t materials = 0;      // surfaces in the pool
        uint64_t triangles = 0;
        uint32_t slices   = 0;       // main-thread visits used
        // ⚠️ TWO DIFFERENT NUMBERS — BackgroundArm's warning, restated because
        // conflating them made its first live run unreadable. `longestHoldMs` is
        // time spent INSIDE the slice, i.e. how long Archicad was actually held;
        // `longestRoundTripMs` is the whole Invoke seen from here, which is
        // mostly WAITING FOR THE GATE and says how busy Archicad is, not what we
        // cost it.
        int64_t  longestHoldMs      = 0;
        int64_t  longestRoundTripMs = 0;
        int64_t  acquireMs = 0;      // the one long call: getting the model
        int64_t  elapsedMs = 0;
        // How long the worker spent WAITING for the render thread to drain,
        // rather than extracting. Large means the GPU side is the bottleneck and
        // the slice budget is not.
        int64_t  throttledMs = 0;
        std::string phase;           // "acquiring" / "extracting" / "idle" / an error

        // ---- live sync (Phase 7) ------------------------------------------
        bool     live      = false;  // the watch loop is running
        uint32_t fullPasses    = 0;
        uint32_t partialPasses = 0;
        uint32_t removed   = 0;      // elements told to disappear from the scene
        uint32_t armed     = 0;      // observers attached to what the 3D view shows
        uint32_t armRefused = 0;     // elements the database would not let us observe
        // ⚠️ ATTACHING IS NOT LISTENING. AttachObserver LINKS an element to this
        // add-on; delivery needs InstallElementObserver, which lives in
        // ChangeTracker and used to be called only from EvP.WatchModel. Live
        // sync armed hundreds of elements and was DEAF, and nothing in `armed`
        // could say so. This is that missing bit, reported.
        bool     handlersInstalled = false;
        uint32_t dirtyPending = 0;   // what the tracker holds for THIS consumer
        // ⚠️ EDIT -> PIXEL, END TO END, and it is the number Phase 7 is judged
        // on. Measured from the last change notification of the batch to the
        // moment its uploads were queued, so it includes the settle wait — which
        // is most of it, deliberately.
        int64_t  lastSyncMs = 0;
        int64_t  lastPassMs = 0;     // how long the last re-extraction itself took
    };

    // Begins (or restarts) a pass. Returns immediately — nothing ACAPI happens
    // on the caller's thread.
    //
    // `full` clears the viewer's scene before the new geometry streams in (the
    // consumer does that at BeginBatch, not EndBatch — SceneCache.cpp says why).
    // `maxSeconds` is a HARD WALL, not a tuning knob: a pass that cannot finish
    // in a sensible time should give up and SAY so rather than keep tapping the
    // main thread indefinitely.
    void Start (bool full = true, int64_t sliceMs = 8, int64_t gapMs = 16,
                int64_t maxSeconds = 300);

    // One pass, then WATCH: arm the observer on what the 3D view shows, register
    // a dirty cursor of our own, and re-extract what changes until stopped.
    //
    // `settleMs` is how quiet the model must go before a re-extraction starts.
    // 400 ms is a starting point, not a measurement — long enough that a drag's
    // per-frame notifications collapse into one pass, short enough to feel live.
    //
    // ⚠⚠ `armObservers` DEFAULTS TO **false**, AND THAT IS A FIX, NOT A
    // CONSERVATIVE DEFAULT (PLAT-RE68). `ACAPI_Element_AttachObserver` does not
    // merely subscribe: it creates a LINK in the project database — the same
    // mechanism as ACAPI_ElementLink_Link, which is why re-attaching returns
    // APIERR_LINKEXIST. Attaching to every visible element therefore MODIFIES THE
    // PROJECT, and the live loop re-arms after every pass, so Archicad saw a
    // continuously edited document and wrote a recovery file over and over for as
    // long as the viewer was open. A VIEWER MUST NEVER DIRTY THE PROJECT.
    //
    // With it false there is no change signal at all, so this runs ONE pass and
    // stops rather than spinning a watch loop that can never fire. Passing true
    // restores the old behaviour AND the old symptom; it exists so the arming
    // path stays reachable for the probe that has to confirm the cause.
    void StartLive (int64_t settleMs = 400, int64_t pollMs = 100,
                    int64_t sliceMs = 8, int64_t gapMs = 16,
                    bool armObservers = false);

    // Stops the pass and JOINS. Safe when not running.
    // ⚠️ ONLY WHERE A BOUNDED BLOCK IS ACCEPTABLE — unload, viewer close, a
    // restart. Called from the main thread while the worker is inside an Invoke,
    // this waits for that Invoke to time out (the worker is waiting for the very
    // thread now sitting in join()), which is why the slice timeout is short.
    void Stop ();

    // Flag only, no join — for main-thread callers that must never block.
    void RequestStop ();

    bool     IsRunning () const { return running_.load (); }
    Progress Snapshot () const;

    // Whether a FULL pass should also cut every storey and union the result.
    //
    // ⚠️ A FLAG RATHER THAN ALWAYS-ON, because the cut is not free: it is a
    // plane test against every triangle in the project plus an O(E^2) union per
    // storey, and a user who never opens the overlay should not pay for it on
    // every refresh.
    //
    // ⚠️ AND IT IS READ AT THE START OF A PASS, NOT PER ELEMENT. Turning it on
    // mid-pass would union the storeys against the elements the pass had reached
    // so far, which is a confident outline of part of a building. The viewer
    // turns it on and asks for a refresh; until that refresh lands the overlay
    // keeps whatever it last had, which is honest.
    void SetStorySlicesWanted (bool wanted) { storySlicesWanted_.store (wanted); }
    bool StorySlicesWanted () const { return storySlicesWanted_.load (); }

private:
    ExtractionWorker () = default;
    ~ExtractionWorker ();
    ExtractionWorker (const ExtractionWorker&) = delete;
    ExtractionWorker& operator= (const ExtractionWorker&) = delete;

    struct Options {
        Mode    mode       = Mode::Once;
        bool    full       = true;
        int64_t sliceMs    = 8;
        int64_t gapMs      = 16;
        int64_t maxSeconds = 300;
        int64_t settleMs   = 400;
        int64_t pollMs     = 100;
        // See StartLive: attaching observers WRITES TO THE PROJECT DATABASE.
        bool    armObservers = false;
    };

    void StartWith (const Options& opt);
    void Run (Options opt);

    // One extraction pass. `filter` empty = the whole model; otherwise only
    // those GUIDs, and anything in the filter the model does NOT contain is
    // pushed as a removal (that is how a delete reaches the scene).
    // Returns false if the pass could not run at all.
    bool RunPass (const Options& opt, bool full, const std::set<std::string>& filter,
                  std::vector<std::string>* extractedGuids);

    // Attach change observers to what we just extracted, in slices.
    // ⚠️ `scope=visible` BY CONSTRUCTION: the list is what the 3D sight actually
    // yielded, so hidden layers and other storeys are never armed. Measured
    // alternative (attach every element in the database) was ~22 minutes on a
    // 12k-element project — see EvP.WatchModel's own warning.
    void ArmObservers (const std::vector<std::string>& guids, const Options& opt);

    void RunLiveLoop (const Options& opt);

    // ---- RE51.B2/B6: the substance join, remembered between passes ----------
    //
    // ⚠️ THE SUBSTANCE WALK IS A FULL-PASS COST AND MUST NOT BECOME A PER-EDIT
    // ONE. Deriving it needs every element's bodies and every body's polygons;
    // that is affordable once per rebuild and is exactly what a partial refresh
    // exists to avoid paying. So a full pass computes it and a partial pass
    // carries it forward.
    //
    // ⚠️ THE NAME IS AN IDENTITY CHECKSUM AND NOTHING ELSE. Archicad RENUMBERS
    // the model's surface pool whenever it rebuilds the 3D, so a remembered
    // index can point at a different surface after an edit -- which would paint
    // one surface with another's substance and look like a shading bug.
    // Comparing the name at that index catches the renumbering and drops the
    // memory instead. This is the standing "names must never decide" rule
    // observed, not bent: the name never contributes to WHAT a substance is,
    // only to whether a previously computed answer still refers to the same
    // surface.
    std::map<int32_t, std::pair<std::string, SurfaceSubstance>> substanceMemory_;

    std::thread        worker_;
    std::atomic<bool>  stopFlag_ { false };
    std::atomic<bool>  running_  { false };
    std::atomic<bool>  storySlicesWanted_ { false };
    mutable std::mutex mutex_;
    Progress           progress_;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
