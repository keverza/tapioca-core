#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyCommands.hpp"
#include "NativeCommands/SunStudyDisplayCommands.hpp"
#include "NativeCommands/SunStudyDisplayPreparation.hpp"
#include "SunStudy/CpuTraversal.hpp"

#include "AddOnCommands.hpp" // ExecuteNativeCommand, CommandNeedsMainThread
#include "ArchViz/ArchVizLog.hpp"
#include "Geometry/MeshStore.hpp" // MeshStore::FullPassInFlight
#include "Python/MainThreadGate.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

constexpr int kSliceSteps = 1;
// StartSunStudy samples and builds the BVH on the main thread; a large model
// takes seconds, so the gate's 30 s default is the floor, not the budget.
constexpr int kMainThreadTimeoutMs = 120000;

std::mutex gMutex;
Status gStatus;
std::thread gThread;
std::shared_ptr<tapioca::SunStudyRunTicket> s_ticket;
std::chrono::steady_clock::time_point s_started;

void SetStage (const std::string& stage)
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (!gStatus.cancelling)
        gStatus.stage = stage;
}

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// One command, on the thread it declares. ⚠️ THE GATE JOB OWNS ITS STATE: a
// timed-out Invoke may still run the job later, so nothing it touches lives on
// this frame.
NativeCommandResult Call (const char* name, const GS::ObjectState& params,
                          std::shared_ptr<tapioca::SunStudyRunTicket> ticket)
{
    if (ticket->cancelled.load ())
        return NativeCommandResult::Failure ("sun study cancelled");
    bool needsMainThread = true;
    CommandNeedsMainThread (GS::String (name), needsMainThread);
    if (!needsMainThread)
        return ExecuteNativeCommand (GS::String (name), params);
    struct Job {
        GS::String name;
        GS::ObjectState params;
        NativeCommandResult result;
        std::shared_ptr<tapioca::SunStudyRunTicket> ticket;
    };
    const auto job = std::make_shared<Job> ();
    job->name = GS::String (name);
    job->params = params;
    job->ticket = std::move (ticket);
    job->result = NativeCommandResult::Failure (GS::UniString ("Tapioca.") + name + " did not run on the main thread");
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Invoke (
            [job] () {
                if (!job->ticket->cancelled.load ())
                    job->result = ExecuteNativeCommand (job->name, job->params);
            },
            kMainThreadTimeoutMs, error))
        return NativeCommandResult::Failure (GS::UniString ("Tapioca.") + name + ": " + error);
    return job->result;
}

// Omitted when empty: an empty list and an absent one mean different role
// tables (SunStudy/SunStudyRoles.hpp).
void AddGuids (GS::ObjectState& params, const char* name, const std::vector<std::string>& guids)
{
    if (guids.empty ())
        return;
    GS::Array<GS::UniString> values;
    for (const auto& guid : guids)
        values.Push (GS::UniString (guid.c_str (), CC_UTF8));
    params.Add (name, values);
}

GS::ObjectState StartParams (const Request& request)
{
    GS::ObjectState params;
    params.Add ("year", (GS::Int32) request.year);
    params.Add ("month", (GS::Int32) request.month);
    params.Add ("day", (GS::Int32) request.day);
    params.Add ("timestep", (GS::Int32) request.timestep);
    params.Add ("hourFrom", (GS::Int32) 0);
    params.Add ("hourTo", (GS::Int32) 24);
    params.Add ("grid", request.grid);
    params.Add ("samples", GS::UniString ("surfaces"));
    params.Add ("domain", GS::UniString ("patch"));
    params.Add ("backend", GS::UniString (request.backend.c_str (), CC_UTF8));
    params.Add ("preset", GS::UniString (request.preset.c_str (), CC_UTF8));
    params.Add ("glassThreshold", request.glassThreshold);
    return params;
}

void Run (Request request, std::shared_ptr<tapioca::SunStudyRunTicket> ticket)
{
    const auto& gCancel = ticket->cancelled;
    const auto started = std::chrono::steady_clock::now ();
    std::string studyId;
    const auto finish = [&] (const std::string& stage, const std::string& error) {
        const bool cancelled = gCancel.load ();
        if (stage == "failed")
            ticket->Cancel (); // invalidate any timed-out gate job before releasing ownership
        if ((cancelled || stage == "failed") && !studyId.empty ())
            evp::sunstudy::SunStudyStore::Get ().Erase (studyId);
        sunfollow::ReleaseManualRun ();
        std::string line;
        {
            std::lock_guard<std::mutex> lock (gMutex);
            gStatus.running = false;
            gStatus.stage = cancelled ? "cancelled" : stage;
            gStatus.error = cancelled ? "" : error;
            gStatus.cancelling = false;
            if (cancelled)
                gStatus.resolvedSteps = gStatus.totalSteps = 0;
            gStatus.studyId = studyId;
            gStatus.wallMs =
                std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
            if (cancelled)
                archviz::ArchVizLog ("sun-cancel source=manual generation=" + std::to_string (gStatus.runs) +
                                     " study=" + studyId +
                                     " acknowledgedMs=" + std::to_string (gStatus.wallMs - gStatus.cancelRequestedMs) +
                                     " lateCompletionDiscarded=true");
            // Requested beside resolved: a GPU request that fell back is
            // visible here and in the HUD, never rewritten to `cpu`.
            if (!studyId.empty ())
                line = "pipeline: stage=sun-hud-backend study=" + studyId + " requested=" + gStatus.requestedBackend +
                       " resolved=" + (gStatus.resolvedBackend.empty () ? "untried" : gStatus.resolvedBackend) +
                       " outcome=" + stage +
                       (gStatus.backendFallbackReason.empty () ? "" : " reason=" + gStatus.backendFallbackReason);
        }
        if (!line.empty ())
            archviz::ArchVizLog (line);
    };
    const auto failed = [&] (const NativeCommandResult& result) { finish ("failed", Utf8 (result.error)); };

    // The previous follower would otherwise see this snapshot and rerun its
    // own study underneath this one.
    SetStage ("snapshot");
    // ⚠️ WAIT FOR THE VIEWER'S FULL PASS HERE, OFF THE MAIN THREAD. Until it
    // publishes, BuildSnapshot finds no complete shared snapshot and walks the
    // whole model itself in ONE main-thread call: 37 s of a blocked Archicad
    // on an 806k-triangle load, with that load stalled behind it (#97). The
    // pass is bounded by its own maxSeconds; Cancel still works here.
    if (MeshStore::Get ().FullPassInFlight ()) {
        SetStage ("waiting");
        const auto waitStarted = std::chrono::steady_clock::now ();
        while (MeshStore::Get ().FullPassInFlight () && !gCancel)
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        if (gCancel)
            return finish ("cancelled", "");
        archviz::ArchVizLog (
            "pipeline: stage=sun-wait-viewer-pass waitedMs=" +
            std::to_string (int64_t (
                std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - waitStarted).count ())));
        SetStage ("snapshot");
    }
    GS::ObjectState snapshotParams;
    snapshotParams.Add ("scope", GS::UniString ("all"));
    snapshotParams.Add ("reuseShared", true);
    if (const auto snapshot = Call ("BuildSnapshot", snapshotParams, ticket); !snapshot.ok)
        return failed (snapshot);
    if (gCancel)
        return finish ("cancelled", "");

    SetStage ("starting");
    GS::ObjectState params = StartParams (request);
    if (request.bounded) {
        params.Add ("clipX", request.centreX);
        params.Add ("clipY", request.centreY);
        params.Add ("clipRadius", request.radius);
    }
    AddGuids (params, "contextElements", request.contextElements);
    AddGuids (params, "ignoredElements", request.ignoredElements);
    // Capture SDK inputs on the host; prepare on this worker with the same token
    // used by ray batches. Timed-out gate jobs own everything they can touch.
    struct CaptureJob {
        std::shared_ptr<const CapturedSunStudyInputs> captured;
        NativeCommandResult result = NativeCommandResult::Failure ("sun capture did not run");
    };
    const auto capture = std::make_shared<CaptureJob> ();
    GS::UniString gateError;
    if (!evp::MainThreadGate::Get ().Invoke (
            [capture, params, ticket] {
                if (!ticket->cancelled.load ())
                    capture->result = CaptureSunStudyInputs (params, capture->captured);
            },
            kMainThreadTimeoutMs, gateError))
        return finish ("failed", Utf8 (gateError));
    if (!capture->result.ok)
        return failed (capture->result);
    const NativeCommandResult start = PrepareCapturedSunStudy (capture->captured, gCancel);
    if (!start.ok)
        return failed (start);
    GS::UniString id;
    start.data.Get ("studyId", id);
    studyId = Utf8 (id);
    {
        std::lock_guard<std::mutex> lock (gMutex);
        gStatus.studyId = studyId;
    }
    bool empty = false;
    start.data.Get ("empty", empty);
    if (empty)
        return finish ("failed", "nothing to study: no sun above the horizon on this date, or no samples");

    SetStage ("calculating");
    for (;;) {
        if (gCancel) {
            return finish ("cancelled", "");
        }
        auto& store = evp::sunstudy::SunStudyStore::Get ();
        size_t advanced = 0;
        std::string error;
        if (!store.Advance (studyId, kSliceSteps, evp::sunstudy::BackgroundTraversalThreads (), 0.001, 0.0, advanced,
                            error, &gCancel))
            return finish ("failed", error);
        if (gCancel)
            return finish ("cancelled", "");
        evp::sunstudy::StudyProgress progress;
        evp::sunstudy::SunStudyBackendReport backend;
        if (!store.Progress (studyId, progress, error))
            return finish ("failed", error);
        store.Backend (studyId, backend, error);
        {
            std::lock_guard<std::mutex> lock (gMutex);
            if (!gStatus.cancelling) {
                gStatus.resolvedSteps = progress.resolvedSteps;
                gStatus.totalSteps = progress.totalSteps;
                gStatus.resolvedBackend = backend.resolved;
                gStatus.backendFallbackReason = backend.fallbackReason;
            }
        }
        if (progress.converged)
            break;
        if (advanced == 0)
            return finish ("failed", "the study stopped before converging");
    }

    // Shown, the follower adopts it and keeps it current through edits.
    SetStage ("showing");
    GS::ObjectState show;
    show.Add ("studyId", id);
    show.Add ("show", true);
    show.Add ("debug", (GS::Int32) 0);
    show.Add ("depth", (GS::Int32) 0);
    if (gCancel)
        return finish ("cancelled", "");
    if (const auto shown = ShowSunStudyForHud (show, ticket); !shown.ok)
        return failed (shown);
    while (ManualSunStudyDisplayStatus ().preparing) {
        if (gCancel) {
            CancelManualSunStudyDisplays ();
            while (ManualSunStudyDisplayBusy ())
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            return finish ("cancelled", "");
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    if (const auto display = ManualSunStudyDisplayStatus (); !display.error.empty ())
        return finish ("failed", display.error);
    // The result's fingerprint, so two runs of one model -- the other backend,
    // another build -- can be compared from the log (#97).
    evp::sunstudy::SunStudyResultSummary summary;
    std::string summaryError;
    if (evp::sunstudy::SunStudyStore::Get ().Summary (studyId, summary, summaryError)) {
        char line[320];
        std::snprintf (line, sizeof (line),
                       "pipeline: stage=sun-result study=%s samples=%zu meanHours=%.9f minHours=%.6f maxHours=%.6f "
                       "fullyLit=%zu fullyShaded=%zu daylightHours=%.6f",
                       studyId.c_str (), summary.count, summary.meanHours, summary.minHours, summary.maxHours,
                       summary.fullyLit, summary.fullyShaded, summary.daylightHours);
        archviz::ArchVizLog (line);
    }
    finish ("done", "");
}

} // namespace

bool Submit (const Request& request, std::string& error)
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (gStatus.running) {
        error = "a sun study is already running";
        return false;
    }
    if (gThread.joinable ())
        gThread.join (); // finished: its last act was to clear `running`
    if (!sunfollow::ReserveManualRun (error))
        return false;
    s_ticket = std::make_shared<tapioca::SunStudyRunTicket> ();
    s_started = std::chrono::steady_clock::now ();
    const uint64_t runs = gStatus.runs + 1;
    gStatus = Status {};
    gStatus.running = true;
    gStatus.stage = "queued";
    gStatus.runs = runs;
    gStatus.requestedBackend = request.backend;
    gThread = std::thread (Run, request, s_ticket);
    return true;
}

void Cancel ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (!gStatus.running || s_ticket == nullptr || !s_ticket->Cancel ())
        return;
    gStatus.cancelRequestedMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - s_started).count ();
    archviz::ArchVizLog (
        "sun-cancel source=manual stage=" + gStatus.stage + " generation=" + std::to_string (gStatus.runs) +
        " study=" + gStatus.studyId + " resolved=" + std::to_string (gStatus.resolvedSteps) + "/" +
        std::to_string (gStatus.totalSteps) + " elapsedMs=" + std::to_string (gStatus.cancelRequestedMs));
    gStatus.cancelling = true;
    gStatus.stage = "cancelling";
}

Status Current ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    return gStatus;
}

Status CurrentJob ()
{
    auto status = Current ();
    const auto follow = sunfollow::State ();
    status.paused = follow.paused;
    status.stale = follow.dirty || follow.paused || (status.running && !follow.studyId.empty ());
    if (status.running)
        return status;
    const bool active = follow.cancelling || follow.workerBusy ||
                        (follow.autoFollow && (follow.dirty || follow.millisecondsUntilStart >= 0));
    if (active || follow.stage == "cancelled" || follow.stage == "failed") {
        status.following = true;
        status.running = active;
        status.cancelling = follow.cancelling;
        status.stage = follow.stage;
        status.error = follow.lastError;
        status.studyId = follow.studyId;
        status.resolvedSteps = follow.resolvedSteps;
        status.totalSteps = follow.totalSteps;
    }
    return status;
}

void CancelActive ()
{
    if (Current ().running)
        Cancel ();
    else
        sunfollow::CancelActiveRun ();
}

bool ResumeFollowing (std::string& error)
{
    error.clear ();
    return sunfollow::Resume (error);
}

void Shutdown ()
{
    Cancel ();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        thread = std::move (gThread);
    }
    if (thread.joinable ())
        thread.join ();
}

} // namespace geomsrv::sunstudyhud
