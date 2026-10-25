#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyHudRunner.hpp"

#include "AddOnCommands.hpp" // ExecuteNativeCommand, CommandNeedsMainThread
#include "Geometry/MeshStore.hpp"
#include "Python/MainThreadGate.hpp"
#include "SunStudy/SunStudyBounds.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

constexpr int kSliceSteps = 4;
// StartSunStudy samples and builds the BVH on the main thread; a large model
// takes seconds, so the gate's 30 s default is the floor, not the budget.
constexpr int kMainThreadTimeoutMs = 120000;

std::mutex gMutex;
Status gStatus;
std::thread gThread;
std::atomic<bool> gCancel { false };

void SetStage (const std::string& stage)
{
    std::lock_guard<std::mutex> lock (gMutex);
    gStatus.stage = stage;
}

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// One command, on the thread it declares. ⚠️ THE GATE JOB OWNS ITS STATE: a
// timed-out Invoke may still run the job later, so nothing it touches lives on
// this frame.
NativeCommandResult Call (const char* name, const GS::ObjectState& params)
{
    bool needsMainThread = true;
    CommandNeedsMainThread (GS::String (name), needsMainThread);
    if (!needsMainThread)
        return ExecuteNativeCommand (GS::String (name), params);
    struct Job {
        GS::String name;
        GS::ObjectState params;
        NativeCommandResult result;
    };
    const auto job = std::make_shared<Job> ();
    job->name = GS::String (name);
    job->params = params;
    job->result = NativeCommandResult::Failure (GS::UniString ("Tapioca.") + name + " did not run on the main thread");
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Invoke ([job] () { job->result = ExecuteNativeCommand (job->name, job->params); },
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
    params.Add ("backend", GS::UniString ("cpu"));
    params.Add ("preset", GS::UniString ("early"));
    params.Add ("glassThreshold", 0.4);
    return params;
}

void Run (Request request)
{
    const auto started = std::chrono::steady_clock::now ();
    std::string studyId;
    const auto finish = [&] (const std::string& stage, const std::string& error) {
        std::lock_guard<std::mutex> lock (gMutex);
        gStatus.running = false;
        gStatus.stage = stage;
        gStatus.error = error;
        gStatus.studyId = studyId;
        gStatus.wallMs =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    };
    const auto failed = [&] (const NativeCommandResult& result) { finish ("failed", Utf8 (result.error)); };

    // The previous follower would otherwise see this snapshot and rerun its
    // own study underneath this one.
    SetStage ("snapshot");
    if (const auto paused = Call ("PauseSunStudyFollowing", GS::ObjectState ()); !paused.ok)
        return failed (paused);
    GS::ObjectState snapshotParams;
    snapshotParams.Add ("scope", GS::UniString ("all"));
    snapshotParams.Add ("reuseShared", true);
    if (const auto snapshot = Call ("BuildSnapshot", snapshotParams); !snapshot.ok)
        return failed (snapshot);
    if (gCancel)
        return finish ("cancelled", "");

    SetStage ("starting");
    GS::ObjectState params = StartParams (request);
    std::vector<std::string> analysis, context = request.contextElements;
    if (request.bounded) {
        const auto snapshot = MeshStore::Get ().Current ();
        if (snapshot == nullptr)
            return finish ("failed", "no model snapshot to bound");
        auto roles = evp::sunstudy::SplitByCylinder (*snapshot, { request.centreX, request.centreY, request.radius },
                                                     request.contextElements, request.ignoredElements);
        if (roles.analysis.empty ())
            return finish ("failed", "no analysed element reaches into the analysis cylinder");
        analysis = std::move (roles.analysis);
        context = std::move (roles.context);
    }
    AddGuids (params, "analysisElements", analysis);
    AddGuids (params, "contextElements", context);
    AddGuids (params, "ignoredElements", request.ignoredElements);
    const NativeCommandResult start = Call ("StartSunStudy", params);
    if (!start.ok)
        return failed (start);
    GS::UniString id;
    start.data.Get ("studyId", id);
    studyId = Utf8 (id);
    bool empty = false;
    start.data.Get ("empty", empty);
    if (empty)
        return finish ("failed", "nothing to study: no sun above the horizon on this date, or no samples");

    SetStage ("calculating");
    for (;;) {
        if (gCancel) {
            GS::ObjectState cancel;
            cancel.Add ("studyId", id);
            Call ("CancelSunStudy", cancel);
            return finish ("cancelled", "");
        }
        GS::ObjectState advance;
        advance.Add ("studyId", id);
        advance.Add ("maxSteps", (GS::Int32) kSliceSteps);
        advance.Add ("maxParallel", (GS::Int32) 0);
        const NativeCommandResult state = Call ("AdvanceSunStudy", advance);
        if (!state.ok)
            return failed (state);
        GS::Int32 resolved = 0, total = 0, advanced = 0;
        bool converged = false;
        state.data.Get ("resolvedSteps", resolved);
        state.data.Get ("totalSteps", total);
        state.data.Get ("advanced", advanced);
        state.data.Get ("converged", converged);
        {
            std::lock_guard<std::mutex> lock (gMutex);
            gStatus.resolvedSteps = size_t (resolved);
            gStatus.totalSteps = size_t (total);
        }
        if (converged)
            break;
        if (advanced == 0)
            return finish ("failed", "the study stopped before converging (" + std::to_string (resolved) + " of " +
                                         std::to_string (total) + " steps)");
    }

    // Shown, the follower adopts it and keeps it current through edits.
    SetStage ("showing");
    GS::ObjectState show;
    show.Add ("studyId", id);
    show.Add ("show", true);
    show.Add ("debug", (GS::Int32) 0);
    show.Add ("depth", (GS::Int32) 0);
    if (const auto shown = Call ("ShowSunStudy", show); !shown.ok)
        return failed (shown);
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
    gCancel = false;
    const uint64_t runs = gStatus.runs + 1;
    gStatus = Status {};
    gStatus.running = true;
    gStatus.stage = "queued";
    gStatus.runs = runs;
    gThread = std::thread (Run, request);
    return true;
}

void Cancel ()
{
    gCancel = true;
}

Status Current ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    return gStatus;
}

void Shutdown ()
{
    gCancel = true;
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        thread = std::move (gThread);
    }
    if (thread.joinable ())
        thread.join ();
}

} // namespace geomsrv::sunstudyhud
