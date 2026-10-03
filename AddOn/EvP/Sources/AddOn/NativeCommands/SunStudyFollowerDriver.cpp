#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyFollowerDriver.hpp"

#include "AddOnCommands.hpp" // ExecuteNativeCommand -- the one implementation
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "ArchViz/CameraWake.hpp"
#include "NativeCommands/SunStudyCommands.hpp"
#include "NativeCommands/SelectionSetStore.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyAdvanceWorker.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyTaskWorker.hpp"
#include "SunStudy/SunStudyRefreshSchedule.hpp"
#include "Geometry/MeshStore.hpp"
#include "Python/MainThreadGate.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>

namespace geomsrv {
namespace sunfollow {

namespace {

using evp::sunstudy::SunStudyDependencySignature;
using evp::sunstudy::SunStudyDirtyReason;
using evp::sunstudy::SunStudyFollower;
using evp::sunstudy::SunStudyFollowState;

// Host policy and ACAPI stay on the main thread. State/Disable may be called
// from a command worker; the calculation worker owns no driver state or SDK data.
std::mutex gMutex;

SunStudyFollower gFollower;
ActiveSunStudyConfig gConfig;
bool gAutoFollow = false;

UINT_PTR gTimer = 0;

// How often the world is re-examined. ⚠️ NOT THE DEBOUNCE, AND MUCH SHORTER THAN
// IT: the quiet period is measured in the follower from real timestamps, so this
// only decides the granularity with which it is noticed. 200 ms keeps the
// hide-on-edit within a couple of frames while asking Archicad for its place
// settings five times a second, which is nothing.
constexpr UINT kTickMs = 200;

// The last genuine geometry edit ModelWatch reported that this driver has acted
// on. ⚠️ MeshStore's SNAPSHOT IS NOT REPUBLISHED BY AN ARCHICAD EDIT -- only
// Tapioca.BuildSnapshot does that -- so without this counter the geometry half
// of the signature would never move and the follower would never fire.
uint32_t gSeenGeometryEdits = 0;

// What the run currently in flight was started for.
uint64_t gRunGeneration = 0;
SunStudyDependencySignature gRunSignature;
std::string gRunStudyId;

bool gLastShouldDisplay = false;

uint64_t gStarts = 0;
uint64_t gAccepted = 0;
uint64_t gDiscarded = 0;
uint64_t gReruns = 0;
uint64_t gSnapshotRebuilds = 0;
std::string gLastError;
uint64_t s_sessionGeneration = 1;
uint64_t s_runRevision = 0;
uint64_t s_cancelledRuns = 0;
uint64_t s_sliceSubmissions = 0;
uint64_t s_sliceCompletions = 0;
evp::sunstudy::StudyProgress s_runProgress;
std::string s_tickThread;
std::string s_workerThread;
evp::sunstudy::SunStudyRefreshSchedule s_refreshSchedule;
std::string s_stage = "idle";
bool s_navigationDeferred = false;
std::atomic<bool> s_completionTickPending { false };
std::atomic<uint64_t> s_completionWakes { 0 };
std::atomic<uint64_t> s_completionWakeFailures { 0 };
std::shared_ptr<const evp::sunstudy::StudyRecord> s_reuseRecord;
int64_t s_phaseStartedMs = 0;

void WakeFollower (uint64_t sessionGeneration)
{
    if (s_completionTickPending.exchange (true))
        return;
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Post (
            [sessionGeneration] () {
                s_completionTickPending.store (false);
                if (SessionGeneration () == sessionGeneration)
                    Tick ();
            },
            error)) {
        s_completionTickPending.store (false);
        ++s_completionWakeFailures;
    }
    else
        ++s_completionWakes;
}

struct PreparationResult {
    NativeCommandResult result;
    std::string studyId;
    uint64_t revision = 0;
};
std::shared_ptr<PreparationResult> s_preparation;

evp::sunstudy::SunStudyTaskWorker& PreparationWorker ()
{
    static auto* worker = new evp::sunstudy::SunStudyTaskWorker ();
    return *worker;
}

bool NavigationActive ()
{
    return archviz::camerawake::Navigating (250) || (::GetAsyncKeyState (VK_MBUTTON) & 0x8000) != 0 ||
           (::GetAsyncKeyState (VK_RBUTTON) & 0x8000) != 0;
}

evp::sunstudy::SunStudyAdvanceWorker& AdvanceWorker ()
{
    // Explicitly joined on quit/unload, not by static destruction under the
    // loader lock. Construction is cheap; its thread starts on first Submit.
    static auto* worker = new evp::sunstudy::SunStudyAdvanceWorker ();
    return *worker;
}

int64_t NowMs ()
{
    return static_cast<int64_t> (::GetTickCount64 ());
}

void Mix (uint64_t& hash, uint64_t value)
{
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= 1099511628211ull;
    }
}

// A picked element list, order- and spelling-independent: the same elements
// picked in another order, or braced differently, are the same study.
void MixGuids (uint64_t& hash, const std::vector<std::string>& guids)
{
    std::vector<std::string> canonical;
    canonical.reserve (guids.size ());
    for (const std::string& guid : guids)
        canonical.push_back (evp::sunstudy::CanonicalGuid (guid));
    std::sort (canonical.begin (), canonical.end ());
    canonical.erase (std::unique (canonical.begin (), canonical.end ()), canonical.end ());
    Mix (hash, canonical.size ());
    for (const std::string& guid : canonical) {
        for (const char c : guid)
            Mix (hash, static_cast<uint8_t> (c));
        Mix (hash, 0xffull); // separator: {AB,C} is not {A,BC}
    }
}

void MixDouble (uint64_t& hash, double value)
{
    uint64_t bits = 0;
    static_assert (sizeof (bits) == sizeof (value), "a double is eight bytes");
    std::memcpy (&bits, &value, sizeof (bits));
    Mix (hash, bits);
}

// The sun the study would resolve, as a hash of the INPUTS that decide it.
//
// ⚠️ THE INPUTS, NOT SunSeries::Version(). That version hashes the resolved step
// LIST, and resolving it costs one ACAPI_GeoLocation_CalcSunOnPlace per timestep
// -- forty-nine host calls to answer "has the sun changed?" five times a second.
// These are exactly the values StartSunStudy feeds into that loop, taken from
// the same API_PlaceInfo it reads, so the two cannot disagree about what a
// different sun is.
//
// ⚠️ AND NOTHING FROM THE 3D WINDOW'S PROJECTION IS IN IT. Archicad raises its
// environment notification on every orbit; a signature that included the
// viewer's sun vector would make navigation restart the analysis, which is the
// 37-rebuild bug wearing a different hat.
uint64_t SunInputHash (const API_PlaceInfo& place, const ActiveSunStudyConfig& config)
{
    uint64_t hash = 1469598103934665603ull;
    MixDouble (hash, place.latitude);
    MixDouble (hash, place.longitude);
    MixDouble (hash, place.altitude);
    Mix (hash, static_cast<uint64_t> (place.sumTime));
    Mix (hash, static_cast<uint64_t> (place.timeZoneInMinutes));
    MixDouble (hash, place.north);
    Mix (hash, static_cast<uint64_t> (config.year));
    Mix (hash, static_cast<uint64_t> (config.month));
    Mix (hash, static_cast<uint64_t> (config.day));
    Mix (hash, static_cast<uint64_t> (config.timestep));
    Mix (hash, static_cast<uint64_t> (config.hourFrom));
    Mix (hash, static_cast<uint64_t> (config.hourTo));
    MixDouble (hash, config.minAltitudeDeg);
    return hash;
}

uint64_t SamplingHash (const ActiveSunStudyConfig& config)
{
    uint64_t hash = 1469598103934665603ull;
    MixDouble (hash, config.grid);
    // The domain dices the surfaces differently, so it IS a sampling input.
    Mix (hash, config.patchDomain ? 1ull : 0ull);
    Mix (hash, config.preset == "late" ? 1ull : 0ull);
    MixDouble (hash, config.glassThreshold);
    Mix (hash, config.analysisRestricted ? 1ull : 0ull);
    // Which elements are measured and which only cast shadow are sampling
    // inputs too: re-picking them must re-measure.
    MixGuids (hash, config.analysisElements);
    MixGuids (hash, config.contextElements);
    MixGuids (hash, config.ignoredElements);
    // ⚠️ THE DISPLAY MODE IS DELIBERATELY ABSENT. Switching from `hours` to
    // `cell checker` changes no measurement; including it would recompute a
    // whole study to change a colour, which is precisely what the atlas design
    // exists to avoid.
    return hash;
}

// geometry + sun + sampling, as the world stands. MAIN THREAD (it reads ACAPI's
// place settings).
SunStudyDependencySignature BuildCurrentSignature (const ActiveSunStudyConfig& config)
{
    SunStudyDependencySignature signature;

    const std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
    signature.geometry = snapshot != nullptr ? snapshot->id : 0;

    API_PlaceInfo place = {};
    if (ACAPI_GeoLocation_GetPlaceSets (&place) == NoError)
        signature.sun = SunInputHash (place, config);
    signature.sampling = SamplingHash (config);
    return signature;
}

GS::ObjectState StartParams (const ActiveSunStudyConfig& config)
{
    GS::ObjectState params;
    params.Add ("year", (GS::Int32) config.year);
    params.Add ("month", (GS::Int32) config.month);
    params.Add ("day", (GS::Int32) config.day);
    params.Add ("timestep", (GS::Int32) config.timestep);
    params.Add ("hourFrom", (GS::Int32) config.hourFrom);
    params.Add ("hourTo", (GS::Int32) config.hourTo);
    params.Add ("minAltitudeDeg", config.minAltitudeDeg);
    params.Add ("grid", config.grid);
    // ⚠️ SURFACES, ALWAYS. It is the only mode that builds an atlas, and the
    // display path refuses a study without one -- so a follower that inherited
    // `ground` would rerun for ever and never show anything.
    params.Add ("samples", GS::UniString ("surfaces"));
    // ⚠️ THE ADOPTED STUDY'S DOMAIN, SENT EXPLICITLY. StartSunStudy defaults to
    // `triangle`, so leaving this out reran every patch study as a triangle one.
    params.Add ("domain", GS::UniString (config.patchDomain ? "patch" : "triangle"));
    if (!config.preset.empty ()) {
        params.Add ("preset", GS::UniString (config.preset.c_str (), CC_UTF8));
        params.Add ("glassThreshold", config.glassThreshold);
    }
    // ⚠️ THE ROLES, SENT ON EVERY RERUN. StartSunStudy with no lists analyses
    // every element, so dropping them would turn the context into analysis.
    GS::Array<GS::UniString> analysis;
    for (const std::string& guid : config.analysisElements)
        analysis.Push (GS::UniString (guid.c_str (), CC_UTF8));
    GS::Array<GS::UniString> context;
    for (const std::string& guid : config.contextElements)
        context.Push (GS::UniString (guid.c_str (), CC_UTF8));
    if (!config.selectionBinding.analysisSet.empty ())
        params.Add ("analysisSelectionSet", GS::UniString (config.selectionBinding.analysisSet.c_str (), CC_UTF8));
    else if (!analysis.IsEmpty () || config.analysisRestricted)
        params.Add ("analysisElements", analysis);
    if (!config.selectionBinding.contextSet.empty ())
        params.Add ("contextSelectionSet", GS::UniString (config.selectionBinding.contextSet.c_str (), CC_UTF8));
    else if (!context.IsEmpty ())
        params.Add ("contextElements", context);
    GS::Array<GS::UniString> ignored;
    for (const std::string& guid : config.ignoredElements)
        ignored.Push (GS::UniString (guid.c_str (), CC_UTF8));
    if (!config.selectionBinding.ignoredSet.empty ())
        params.Add ("ignoredSelectionSet", GS::UniString (config.selectionBinding.ignoredSet.c_str (), CC_UTF8));
    else if (!ignored.IsEmpty ())
        params.Add ("ignoredElements", ignored);
    return params;
}

void HideOverlay ()
{
    GS::ObjectState params;
    params.Add ("show", false);
    params.Add ("follow", false); // internal hide must not recursively lock Disable
    ExecuteNativeCommand ("ShowSunStudy", params);
}

void Log (const std::string& line)
{
    archviz::ArchVizLog ("sun follow: " + line);
}

void RefreshRoleSelections ()
{
    auto& binding = gConfig.selectionBinding;
    if (binding.generation == 0)
        return;
    const auto& store = SelectionSetStore::Get ();
    if (binding.generation == store.Generation () && binding.revision == store.Revision ())
        return;
    binding.revision = store.Revision ();
    uint64_t generation = store.Generation ();
    const auto read = [&store, &generation] (const std::string& name) {
        std::vector<std::string> guids;
        if (name.empty ())
            return guids;
        const GS::UniString setName (name.c_str (), CC_UTF8);
        if (!store.IsDeclared (setName))
            generation = 0;
        else
            for (const auto& guid : store.Values (setName))
                guids.emplace_back (guid.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        return guids;
    };
    const auto context = read (binding.contextSet), ignored = read (binding.ignoredSet),
               analysis = read (binding.analysisSet);
    const auto refreshed = binding.Refresh (generation, context, ignored, gConfig.contextElements,
                                            gConfig.ignoredElements, analysis, &gConfig.analysisElements);
    if (refreshed == evp::sunstudy::SelectionBindingRefresh::Detached)
        Log ("role selection source detached -- retaining the last exclusions");
    else if (refreshed == evp::sunstudy::SelectionBindingRefresh::Changed)
        Log ("role selections changed context=" + std::to_string (gConfig.contextElements.size ()) +
             " ignored=" + std::to_string (gConfig.ignoredElements.size ()));
}

void RetireRun (const char* reason, bool cancelled = true)
{
    PreparationWorker ().Cancel ();
    if (s_preparation != nullptr) {
        if (cancelled)
            ++s_cancelledRuns;
        s_preparation.reset ();
    }
    AdvanceWorker ().Cancel ();
    if (gRunStudyId.empty ())
        return;
    if (cancelled)
        ++s_cancelledRuns;
    evp::sunstudy::SunStudyStore::Get ().Erase (gRunStudyId, s_runRevision);
    Log ("retired study=" + gRunStudyId + " -- " + reason);
    gRunStudyId.clear ();
    s_runRevision = 0;
}

void LogHostPhase (const char* phase, int64_t started)
{
    Log (std::string (phase) + " thread=" + std::to_string (::GetCurrentThreadId ()) +
         " wallMs=" + std::to_string (NowMs () - started));
}

double ReadyWaitMs (std::chrono::steady_clock::time_point readyAt)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - readyAt).count ();
}

// Refresh MeshStore so the geometry half of the signature can move.
//
// Called after geometry notifications settle, outside navigation. Known pending
// edits prevent accepting any completion even before MeshStore's id changes.
bool RefreshSnapshot ()
{
    // ⚠️ FALSE MEANS THE SIGNAL MUST NOT BE CONSUMED. An extraction already in
    // flight will finish and this tick simply comes round again in 200 ms; a
    // driver that marked the edit as seen anyway would drop it, and the study
    // would go on describing a building that had changed -- the exact silent
    // failure the whole follower exists to prevent.
    if (archviz::ExtractionWorker::Get ().IsRunning ())
        return false;

    const int64_t started = NowMs ();
    const NativeCommandResult result = ExecuteNativeCommand ("BuildSnapshot", GS::ObjectState ());
    LogHostPhase ("snapshot", started);
    ++gSnapshotRebuilds;
    if (!result.ok) {
        gLastError = std::string (result.error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        return false;
    }
    return true;
}

void StartReplacement (const SunStudyDependencySignature& signature, int64_t now)
{
    // ⚠️ THE GENERATION IS TAKEN BEFORE THE COMMAND RUNS. StartSunStudy is not
    // instantaneous, and a generation read afterwards could already belong to an
    // edit that arrived while it worked -- which would make a superseded run look
    // current, the one failure this whole mechanism exists to prevent.
    gRunGeneration = gFollower.NoteStarted (signature, now);
    ++gStarts;
    gRunSignature = signature;
    gRunStudyId.clear ();
    s_runProgress = {};

    const int64_t started = NowMs ();
    s_phaseStartedMs = started;
    std::shared_ptr<const CapturedSunStudyInputs> captured;
    // Never replace the whole atlas with a coarse grid. Unchanged receivers
    // retain their full-resolution day; dirty receivers use the requested grid.
    const NativeCommandResult capture = CaptureSunStudyInputs (StartParams (gConfig), captured);
    LogHostPhase ("sun capture", started);
    if (!capture.ok) {
        s_stage = "failed";
        gLastError = std::string (capture.error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        gFollower.NoteFailed (gRunGeneration, now);
        Log ("capture refused -- " + gLastError);
        return;
    }
    s_preparation = std::make_shared<PreparationResult> ();
    const auto output = s_preparation;
    evp::sunstudy::StudyTaskRequest request;
    request.sessionGeneration = s_sessionGeneration;
    request.runGeneration = gRunGeneration;
    const uint64_t sessionGeneration = s_sessionGeneration;
    request.onReady = [sessionGeneration] () { WakeFollower (sessionGeneration); };
    const auto accepted = gFollower.StudySignature ();
    const auto reuseSource =
        accepted.sun == signature.sun && accepted.sampling == signature.sampling ? s_reuseRecord : nullptr;
    request.execute = [captured, output, reuseSource] (const std::atomic<bool>& cancelled) {
        output->result = PrepareCapturedSunStudy (captured, cancelled, reuseSource);
        if (output->result.ok) {
            GS::UniString id;
            output->result.data.Get ("studyId", id);
            output->studyId = std::string (id.ToCStr (0, MaxUSize, CC_UTF8).Get ());
            output->revision = evp::sunstudy::SunStudyStore::Get ().Revision (output->studyId);
        }
    };
    request.discard = [output] () {
        if (!output->studyId.empty ())
            evp::sunstudy::SunStudyStore::Get ().Erase (output->studyId, output->revision);
    };
    if (!PreparationWorker ().Submit (std::move (request), gLastError)) {
        s_stage = "failed";
        gFollower.NoteFailed (gRunGeneration, now);
        s_preparation.reset ();
        return;
    }
    s_stage = "preparing";
    Log ("preparing generation=" + std::to_string (gRunGeneration) +
         " snapshot=" + std::to_string (signature.geometry));
}

bool PollPreparation (int64_t now)
{
    if (s_preparation == nullptr)
        return true;
    evp::sunstudy::StudyTaskCompletion completion;
    if (!PreparationWorker ().Poll (completion))
        return false;
    const auto output = std::move (s_preparation);
    std::ostringstream thread;
    thread << completion.threadId;
    s_workerThread = thread.str ();
    Log ("prepare workerThread=" + s_workerThread + " wallMs=" + std::to_string (completion.wallMilliseconds) +
         " priorityLowered=" + std::to_string (completion.priorityLowered) +
         " readyWaitMs=" + std::to_string (ReadyWaitMs (completion.readyAt)));
    if (completion.sessionGeneration != s_sessionGeneration || completion.runGeneration != gRunGeneration) {
        ++gDiscarded;
        if (!output->studyId.empty ())
            evp::sunstudy::SunStudyStore::Get ().Erase (output->studyId, output->revision);
        return false;
    }
    if (!completion.error.empty () || !output->result.ok) {
        gLastError = completion.error.empty () ? std::string (output->result.error.ToCStr (0, MaxUSize, CC_UTF8).Get ())
                                               : completion.error;
        gFollower.NoteFailed (gRunGeneration, now);
        s_stage = "failed";
        Log ("preparation refused -- " + gLastError);
        return false;
    }
    gRunStudyId = output->studyId;
    s_runRevision = output->revision;
    s_stage = "calculating";
    return true;
}

void SubmitAdvanceSlice (int64_t now)
{
    evp::sunstudy::AdvanceRequest request;
    request.studyId = gRunStudyId;
    request.sessionGeneration = s_sessionGeneration;
    request.runGeneration = gRunGeneration;
    request.recordRevision = s_runRevision;
    // Host interactivity takes precedence over CPU throughput. Single-thread
    // traversal also avoids creating normal-priority shards from the low-priority
    // worker. Budget is soft: a whole timestep can exceed it.
    request.maxParallel = 1;
    request.maxMilliseconds = 40.0;
    const uint64_t sessionGeneration = s_sessionGeneration;
    request.onReady = [sessionGeneration] () { WakeFollower (sessionGeneration); };
    if (AdvanceWorker ().Submit (request, gLastError))
        ++s_sliceSubmissions;
    else {
        gFollower.NoteFailed (gRunGeneration, now);
        RetireRun ("submission failed", false);
    }
}

void AdvanceOneSlice (int64_t now)
{
    if (gRunStudyId.empty ()) {
        gFollower.NoteFailed (gRunGeneration, now);
        return;
    }

    evp::sunstudy::AdvanceCompletion completion;
    if (!AdvanceWorker ().Poll (completion)) {
        if (!AdvanceWorker ().Busy ())
            SubmitAdvanceSlice (now);
        return;
    }
    ++s_sliceCompletions;
    std::ostringstream thread;
    thread << completion.threadId;
    s_workerThread = thread.str ();
    if (completion.request.sessionGeneration != s_sessionGeneration ||
        completion.request.runGeneration != gRunGeneration || completion.request.studyId != gRunStudyId ||
        completion.request.recordRevision != s_runRevision) {
        ++gDiscarded;
        Log ("discarded a late calculation slice");
        return;
    }
    if (evp::sunstudy::SunStudyStore::Get ().Revision (gRunStudyId) != s_runRevision) {
        ++gDiscarded;
        gLastError = "sun study record changed before publication";
        gFollower.NoteFailed (gRunGeneration, now);
        RetireRun ("record replaced", false);
        return;
    }
    s_runProgress = completion.progress;
    Log ("advance workerThread=" + s_workerThread + " tickThread=" + s_tickThread +
         " wallMs=" + std::to_string (completion.wallMilliseconds) +
         " priorityLowered=" + std::to_string (completion.priorityLowered) +
         " readyWaitMs=" + std::to_string (ReadyWaitMs (completion.readyAt)) +
         " resolved=" + std::to_string (s_runProgress.resolvedSteps) + "/" + std::to_string (s_runProgress.totalSteps));
    if (!completion.succeeded || (!s_runProgress.converged && completion.advanced == 0)) {
        gLastError = completion.error.empty () ? "sun study made no progress" : completion.error;
        gFollower.NoteFailed (gRunGeneration, now);
        Log ("advance failed -- " + gLastError);
        RetireRun ("advance failed", false);
        return;
    }
    if (!s_runProgress.converged) {
        SubmitAdvanceSlice (now);
        return;
    }

    // ---- the result is in; may it be shown? --------------------------------
    //
    // ⚠️ THE DECISION IS THE FOLLOWER'S AND IT IS MADE BEFORE ANYTHING IS DRAWN.
    // A study started on snapshot 41 that completes after the model reached 42
    // completes SUCCESSFULLY, with a plausible result, and painting it would put
    // the old building's sunlight on the new one.
    if (!gFollower.CanPublishResult (gRunGeneration, gRunSignature)) {
        ++gDiscarded;
        Log ("discarded generation=" + std::to_string (gRunGeneration) + " study=" + gRunStudyId +
             " -- the model moved while it ran");
        RetireRun ("completion superseded", false);
        return;
    }

    GS::ObjectState show;
    show.Add ("studyId", GS::UniString (gRunStudyId.c_str (), CC_UTF8));
    show.Add ("show", true);
    // ⚠️ THE DRIVER'S OWN DISPLAY MUST NOT RE-ADOPT. ShowSunStudy arms the
    // follower for a study a person asked to see; a rerun coming back through it
    // would re-adopt its own result, reset the quiet period it was started by,
    // and overwrite the configuration with one derived from itself.
    show.Add ("follow", false);
    show.Add ("preview", false);
    show.Add ("debug", (GS::Int32) gConfig.debug);
    show.Add ("depth", (GS::Int32) gConfig.depth);
    if (gConfig.hoursMax > 0.0)
        show.Add ("hoursMax", gConfig.hoursMax);
    const int64_t started = NowMs ();
    s_stage = "displaying";
    const NativeCommandResult shown = ExecuteNativeCommand ("ShowSunStudy", show);
    LogHostPhase ("display", started);
    bool enqueuedToViewer = false;
    if (shown.ok)
        shown.data.Get ("shown", enqueuedToViewer);
    if (!shown.ok || !enqueuedToViewer) {
        gLastError = shown.ok ? "sun study viewer is no longer running"
                              : std::string (shown.error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        gFollower.NoteFailed (gRunGeneration, now);
        RetireRun ("display failed", false);
        s_stage = "failed";
        return;
    }
    s_reuseRecord = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (gRunStudyId);
    gFollower.NoteCompleted (gRunGeneration, gRunStudyId, gRunSignature, now);
    ++gAccepted;
    ++gReruns;
    Log ("accepted generation=" + std::to_string (gRunGeneration) + " study=" + gRunStudyId +
         " wallMs=" + std::to_string (NowMs () - s_phaseStartedMs) + " -- full-resolution queued for display");
    gRunStudyId.clear ();
    s_stage = "current";
}

// ⚠️ THE LOCK IS ALREADY HELD BY THE CALLER. std::mutex is not recursive, so
// Tick() cannot call the public Disable().
void DisableLocked ()
{
    gAutoFollow = false;
    ++s_sessionGeneration; // queued adoption/completion from a closed session is obsolete
    RetireRun ("following disabled");
    s_reuseRecord.reset ();
    s_runProgress = {};
    s_stage = "idle";
    s_navigationDeferred = false;
    gConfig = ActiveSunStudyConfig {};
    gFollower.Clear ();
    gRunStudyId.clear ();
    if (gTimer != 0) {
        const UINT_PTR timer = gTimer;
        if (evp::MainThreadGate::Get ().IsMainThread ())
            ::KillTimer (nullptr, timer);
        else {
            GS::UniString error;
            evp::MainThreadGate::Get ().Post ([timer] () { ::KillTimer (nullptr, timer); }, error);
        }
        gTimer = 0;
    }
}

void CALLBACK TickProc (HWND, UINT, UINT_PTR, DWORD)
{
    Tick ();
}

// ⚠️ ::SetTimer BINDS THE TIMER TO THE CALLING THREAD'S MESSAGE QUEUE, and only
// Archicad's main thread has one that is pumped. `ShowSunStudy` declares
// NeedsMainThread() == false -- deliberately, it touches no ACAPI -- so the
// in-process dispatcher may run it on a worker, and a timer armed there would
// never fire once. The follower would then sit in Current for ever and follow
// nothing, with every diagnostic reporting that it was armed.
void ArmTimer ()
{
    if (gTimer != 0)
        return;
    if (evp::MainThreadGate::Get ().IsMainThread ()) {
        gTimer = ::SetTimer (nullptr, 0, kTickMs, TickProc);
        return;
    }
    GS::UniString error;
    // Fire-and-forget, and self-contained: the job captures nothing. See the
    // gate's contract note on by-reference captures.
    evp::MainThreadGate::Get ().Post (
        [] () {
            std::lock_guard<std::mutex> lock (gMutex);
            if (gTimer == 0 && gAutoFollow)
                gTimer = ::SetTimer (nullptr, 0, kTickMs, TickProc);
        },
        error);
}

} // namespace

void Adopt (const std::string& studyId, const ActiveSunStudyConfig& config, uint64_t sessionGeneration)
{
    if (!evp::MainThreadGate::Get ().IsMainThread ()) {
        GS::UniString error;
        evp::MainThreadGate::Get ().Post (
            [studyId, config, sessionGeneration] () { Adopt (studyId, config, sessionGeneration); }, error);
        return;
    }
    std::lock_guard<std::mutex> lock (gMutex);
    if (sessionGeneration != s_sessionGeneration || !archviz::modelwatch::Get ().running)
        return;
    if (studyId == gRunStudyId) {
        // The user explicitly takes ownership of this cache; stop scheduling
        // it but do not erase the study just handed to the display consumer.
        AdvanceWorker ().Cancel ();
        gRunStudyId.clear ();
        s_runRevision = 0;
    }
    else
        RetireRun ("manual study adopted");
    gConfig = config;
    gConfig.valid = true;
    gAutoFollow = true;
    gSeenGeometryEdits = archviz::modelwatch::Get ().geometryEdits;
    s_refreshSchedule.Reset (gSeenGeometryEdits);
    s_stage = "current";
    s_reuseRecord = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (studyId);

    const SunStudyDependencySignature signature = BuildCurrentSignature (gConfig);
    gFollower.Adopt (studyId, signature, NowMs ());
    gLastShouldDisplay = true;
    gLastError.clear ();

    ArmTimer ();
    Log ("following study " + studyId + " at snapshot " + std::to_string (signature.geometry));
}

uint64_t SessionGeneration ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    return s_sessionGeneration;
}

void Disable ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    DisableLocked ();
}

void Shutdown ()
{
    Disable ();
    AdvanceWorker ().Shutdown ();
    PreparationWorker ().Shutdown ();
}

void Tick ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (!gAutoFollow || !gConfig.valid)
        return;

    // ---- 0. stop with the watch -------------------------------------------
    //
    // ⚠️ THIS IS ALSO THE TEARDOWN RULE, AND IT HAS TO BE. This timer calls ACAPI
    // through the commands it drives, so it must not outlive the add-on -- the
    // same reason ModelWatch and the selection bridge are stopped by hand in the
    // palette's teardown. That teardown stops the watch FIRST, so a driver that
    // stops with the watch stops before anything it depends on is gone; and
    // following without a change detector is pointless anyway, because nothing
    // would ever tell it the model moved.
    if (!archviz::modelwatch::Get ().running) {
        Log ("stopping -- the model watch is not running");
        DisableLocked ();
        return;
    }

    const int64_t now = NowMs ();
    s_tickThread = std::to_string (::GetCurrentThreadId ());
    // Refresh before signature comparison AND before any completion may publish.
    // Role changes need no geometry capture; the sampling dependency invalidates
    // the old result and schedules one latest-target replacement.
    RefreshRoleSelections ();

    // ---- 1. has Archicad reported a genuine element change? ------------------
    const uint32_t edits = archviz::modelwatch::Get ().geometryEdits;
    int64_t signatureObservedMs = now;
    s_navigationDeferred = NavigationActive ();
    if (s_refreshSchedule.Observe (edits, now)) {
        RetireRun ("geometry changed");
        archviz::ArchVizLog ("pipeline: stage=sun-batch-pending edits=" + std::to_string (edits) +
                             " signals=" + std::to_string (s_refreshSchedule.PendingSignals ()) +
                             " observations=" + std::to_string (s_refreshSchedule.PendingObservations ()) +
                             " quietMs=" + std::to_string (s_refreshSchedule.QuietMilliseconds ()) +
                             " pendingTargets=1");
    }
    if (s_refreshSchedule.Pending ()) {
        s_stage = "waitingForGeometry";
        if (!s_refreshSchedule.Ready (now, s_navigationDeferred))
            return;
        // Cancellation is cooperative. Do not capture intermediate snapshots
        // while an old preparation/timestep is still draining on its worker.
        if (AdvanceWorker ().Busy () || PreparationWorker ().Busy ())
            return;
        if (!RefreshSnapshot ())
            return; // never accept an old result while a known edit awaits capture
        signatureObservedMs = s_refreshSchedule.LastEditMs ();
        archviz::ArchVizLog ("pipeline: stage=sun-batch-capture edits=" + std::to_string (edits) +
                             " signals=" + std::to_string (s_refreshSchedule.PendingSignals ()) +
                             " observations=" + std::to_string (s_refreshSchedule.PendingObservations ()) +
                             " quietMs=" + std::to_string (s_refreshSchedule.QuietMilliseconds ()) + " waitMs=" +
                             std::to_string (NowMs () - s_refreshSchedule.BatchStartedMs ()) + " pendingTargets=0");
        gSeenGeometryEdits = edits;
        s_refreshSchedule.Complete ();
    }

    // ---- 2. observe the world, once, through one path ----------------------
    // ⚠️ EVERY TICK, NOT ONLY THE INTERESTING ONES. An environment-only tick
    // produces an IDENTICAL signature, so it costs a comparison and changes
    // nothing -- which is exactly the property that keeps navigation free, and
    // it is cheaper to guarantee with one path than with a special case.
    const SunStudyDependencySignature world = BuildCurrentSignature (gConfig);
    // Geometry has already settled before capture; do not debounce it a second
    // time. Sun/sampling-only changes still use the follower's normal delay.
    if (gFollower.Observe (world, signatureObservedMs))
        Log (gFollower.Describe ());
    if ((!gRunStudyId.empty () || s_preparation != nullptr) &&
        (gRunGeneration != gFollower.Generation () || gRunSignature != world))
        RetireRun ("inputs superseded");

    // ---- 3. the overlay STAYS UP ------------------------------------------
    //
    // ⚠️ THIS USED TO HIDE IT AND THAT WAS THE WRONG SHAPE. One wall moving
    // does not invalidate the sunlight on the rest of a building, and blanking
    // the whole analysis to express "part of this is stale" throws away the
    // correct majority of it at exactly the moment the user is reading it.
    // Staleness is a property of a REGION and belongs to the region.
    //
    // The overlay comes off only when there is genuinely nothing to show -- the
    // user turned it off, the viewer closed, the configuration went away, or the
    // cache cannot be mapped to the scene at all. The first two are
    // `ShowSunStudy show=false` from elsewhere; the last is a refusal the
    // renderer already reports through elementsAttached.
    const bool display = gFollower.HasRenderableCache ();
    if (gLastShouldDisplay && !display) {
        HideOverlay ();
        Log ("overlay hidden -- nothing renderable remains");
    }
    gLastShouldDisplay = display;

    // ---- 4. act on the decision --------------------------------------------
    switch (gFollower.State ()) {
        case SunStudyFollowState::DirtyVisible:
            if (gFollower.ShouldStart (now) && !AdvanceWorker ().Busy () && !PreparationWorker ().Busy () &&
                !s_navigationDeferred)
                StartReplacement (world, now);
            break;
        case SunStudyFollowState::UpdatingVisible:
            if (!s_navigationDeferred && PollPreparation (now))
                AdvanceOneSlice (now);
            break;
        case SunStudyFollowState::NoStudy:
        case SunStudyFollowState::Current:
        case SunStudyFollowState::Starting:
        case SunStudyFollowState::Failed:
            break;
    }
}

FollowerStats State ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    FollowerStats stats;
    stats.state = gFollower.State ();
    stats.dirtyReason = gFollower.DirtyReason ();
    stats.autoFollow = gAutoFollow;
    stats.dirty = gFollower.IsDirty ();
    stats.generation = gFollower.Generation ();
    stats.studyId = gFollower.StudyId ();
    stats.studySnapshot = gFollower.StudySignature ().geometry;
    stats.sceneSnapshot = gFollower.WorldSignature ().geometry;
    stats.starts = gStarts;
    stats.acceptedCompletions = gAccepted;
    stats.discardedCompletions = gDiscarded;
    stats.automaticReruns = gReruns;
    stats.snapshotRebuilds = gSnapshotRebuilds;
    stats.sessionGeneration = s_sessionGeneration;
    stats.cancelledRuns = s_cancelledRuns;
    stats.sliceSubmissions = s_sliceSubmissions;
    stats.sliceCompletions = s_sliceCompletions;
    stats.workerBusy = AdvanceWorker ().Busy () || PreparationWorker ().Busy ();
    stats.stage = s_stage;
    stats.navigationDeferred = s_navigationDeferred;
    stats.completionWakes = s_completionWakes.load ();
    stats.completionWakeFailures = s_completionWakeFailures.load ();
    evp::sunstudy::StudyProgress progress = s_runProgress;
    std::string error;
    if (!gRunStudyId.empty ())
        evp::sunstudy::SunStudyStore::Get ().Progress (gRunStudyId, progress, error, s_runRevision);
    stats.resolvedSteps = progress.resolvedSteps;
    stats.totalSteps = progress.totalSteps;
    stats.tickThread = s_tickThread;
    stats.workerThread = s_workerThread;
    stats.lastError = gLastError;
    stats.description = gFollower.Describe ();
    stats.millisecondsUntilStart = s_refreshSchedule.Pending () ? s_refreshSchedule.MillisecondsUntilReady (NowMs ())
                                                                : gFollower.MillisecondsUntilStart (NowMs ());
    return stats;
}

} // namespace sunfollow
} // namespace geomsrv
