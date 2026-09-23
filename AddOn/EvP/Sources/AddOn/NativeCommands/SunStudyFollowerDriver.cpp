#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyFollowerDriver.hpp"

#include "AddOnCommands.hpp" // ExecuteNativeCommand -- the one implementation
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "Geometry/MeshStore.hpp"
#include "Python/MainThreadGate.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace geomsrv {
namespace sunfollow {

namespace {

using evp::sunstudy::SunStudyDependencySignature;
using evp::sunstudy::SunStudyDirtyReason;
using evp::sunstudy::SunStudyFollower;
using evp::sunstudy::SunStudyFollowState;

// ⚠️ EVERYTHING HERE IS MAIN-THREAD ONLY AND THE MUTEX GUARDS THE READER, NOT
// THE WRITER. Tick() runs on a ::SetTimer callback, which is the main thread;
// State() is called from a bus command, which may be another. Only the second
// needs protecting.
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
    // ⚠️ THE ROLES, SENT ON EVERY RERUN. StartSunStudy with no lists analyses
    // every element, so dropping them would turn the context into analysis.
    GS::Array<GS::UniString> analysis;
    for (const std::string& guid : config.analysisElements)
        analysis.Push (GS::UniString (guid.c_str (), CC_UTF8));
    GS::Array<GS::UniString> context;
    for (const std::string& guid : config.contextElements)
        context.Push (GS::UniString (guid.c_str (), CC_UTF8));
    if (!analysis.IsEmpty ())
        params.Add ("analysisElements", analysis);
    if (!context.IsEmpty ())
        params.Add ("contextElements", context);
    GS::Array<GS::UniString> ignored;
    for (const std::string& guid : config.ignoredElements)
        ignored.Push (GS::UniString (guid.c_str (), CC_UTF8));
    if (!ignored.IsEmpty ())
        params.Add ("ignoredElements", ignored);
    return params;
}

void HideOverlay ()
{
    GS::ObjectState params;
    params.Add ("show", false);
    ExecuteNativeCommand ("ShowSunStudy", params);
}

void Log (const std::string& line)
{
    archviz::ArchVizLog ("sun follow: " + line);
}

// Refresh MeshStore so the geometry half of the signature can move.
//
// ⚠️ PROMPTLY ON A REPORTED EDIT, NOT ON THE DEBOUNCE, because the rebuild is
// what CONFIRMS the edit -- until the snapshot advances, nothing downstream can
// tell that the model moved, and the overlay would go on showing a heat map of a
// building that has changed. The quiet period governs the replacement STUDY, not
// this.
bool RefreshSnapshot ()
{
    // ⚠️ FALSE MEANS THE SIGNAL MUST NOT BE CONSUMED. An extraction already in
    // flight will finish and this tick simply comes round again in 200 ms; a
    // driver that marked the edit as seen anyway would drop it, and the study
    // would go on describing a building that had changed -- the exact silent
    // failure the whole follower exists to prevent.
    if (archviz::ExtractionWorker::Get ().IsRunning ())
        return false;

    const NativeCommandResult result = ExecuteNativeCommand ("BuildSnapshot", GS::ObjectState ());
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
    gRunSignature = signature;
    gRunStudyId.clear ();
    ++gStarts;

    const NativeCommandResult result = ExecuteNativeCommand ("StartSunStudy", StartParams (gConfig));
    if (!result.ok) {
        gLastError = std::string (result.error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        gFollower.NoteFailed (gRunGeneration, now);
        Log ("start refused -- " + gLastError);
        return;
    }
    GS::UniString id;
    result.data.Get ("studyId", id);
    gRunStudyId = std::string (id.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    Log ("started generation=" + std::to_string (gRunGeneration) + " study=" + gRunStudyId +
         " snapshot=" + std::to_string (signature.geometry));
}

void AdvanceOneSlice (int64_t now)
{
    if (gRunStudyId.empty ()) {
        gFollower.NoteFailed (gRunGeneration, now);
        return;
    }

    GS::ObjectState params;
    params.Add ("studyId", GS::UniString (gRunStudyId.c_str (), CC_UTF8));
    // One bounded slice. See the header: this is a scheduler.
    params.Add ("maxSteps", (GS::Int32) 8);
    const NativeCommandResult result = ExecuteNativeCommand ("AdvanceSunStudy", params);
    if (!result.ok) {
        gLastError = std::string (result.error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        gFollower.NoteFailed (gRunGeneration, now);
        Log ("advance failed -- " + gLastError);
        return;
    }

    bool converged = false;
    result.data.Get ("converged", converged);
    if (!converged)
        return;

    // ---- the result is in; may it be shown? --------------------------------
    //
    // ⚠️ THE DECISION IS THE FOLLOWER'S AND IT IS MADE BEFORE ANYTHING IS DRAWN.
    // A study started on snapshot 41 that completes after the model reached 42
    // completes SUCCESSFULLY, with a plausible result, and painting it would put
    // the old building's sunlight on the new one.
    if (!gFollower.NoteCompleted (gRunGeneration, gRunStudyId, gRunSignature, now)) {
        ++gDiscarded;
        Log ("discarded generation=" + std::to_string (gRunGeneration) + " study=" + gRunStudyId +
             " -- the model moved while it ran");
        GS::ObjectState cancel;
        cancel.Add ("studyId", GS::UniString (gRunStudyId.c_str (), CC_UTF8));
        ExecuteNativeCommand ("CancelSunStudy", cancel);
        gRunStudyId.clear ();
        return;
    }

    ++gAccepted;
    ++gReruns;

    GS::ObjectState show;
    show.Add ("studyId", GS::UniString (gRunStudyId.c_str (), CC_UTF8));
    show.Add ("show", true);
    // ⚠️ THE DRIVER'S OWN DISPLAY MUST NOT RE-ADOPT. ShowSunStudy arms the
    // follower for a study a person asked to see; a rerun coming back through it
    // would re-adopt its own result, reset the quiet period it was started by,
    // and overwrite the configuration with one derived from itself.
    show.Add ("follow", false);
    show.Add ("debug", (GS::Int32) gConfig.debug);
    show.Add ("depth", (GS::Int32) gConfig.depth);
    if (gConfig.hoursMax > 0.0)
        show.Add ("hoursMax", gConfig.hoursMax);
    const NativeCommandResult shown = ExecuteNativeCommand ("ShowSunStudy", show);
    if (!shown.ok)
        gLastError = std::string (shown.error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    Log ("accepted generation=" + std::to_string (gRunGeneration) + " study=" + gRunStudyId + " -- displayed");
    gRunStudyId.clear ();
}

// ⚠️ THE LOCK IS ALREADY HELD BY THE CALLER. std::mutex is not recursive, so
// Tick() cannot call the public Disable().
void DisableLocked ()
{
    gAutoFollow = false;
    gConfig = ActiveSunStudyConfig {};
    gFollower.Clear ();
    gRunStudyId.clear ();
    if (gTimer != 0) {
        ::KillTimer (nullptr, gTimer);
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

void Adopt (const std::string& studyId, const ActiveSunStudyConfig& config)
{
    std::lock_guard<std::mutex> lock (gMutex);
    gConfig = config;
    gConfig.valid = true;
    gAutoFollow = true;
    gSeenGeometryEdits = archviz::modelwatch::Get ().geometryEdits;

    const SunStudyDependencySignature signature = BuildCurrentSignature (gConfig);
    gFollower.Adopt (studyId, signature, NowMs ());
    gLastShouldDisplay = true;
    gLastError.clear ();

    ArmTimer ();
    Log ("following study " + studyId + " at snapshot " + std::to_string (signature.geometry));
}

void Disable ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    DisableLocked ();
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

    // ---- 1. has Archicad reported a genuine element change? ------------------
    const uint32_t edits = archviz::modelwatch::Get ().geometryEdits;
    if (edits != gSeenGeometryEdits && RefreshSnapshot ())
        gSeenGeometryEdits = edits;

    // ---- 2. observe the world, once, through one path ----------------------
    // ⚠️ EVERY TICK, NOT ONLY THE INTERESTING ONES. An environment-only tick
    // produces an IDENTICAL signature, so it costs a comparison and changes
    // nothing -- which is exactly the property that keeps navigation free, and
    // it is cheaper to guarantee with one path than with a special case.
    const SunStudyDependencySignature world = BuildCurrentSignature (gConfig);
    if (gFollower.Observe (world, now))
        Log (gFollower.Describe ());

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
            if (gFollower.ShouldStart (now))
                StartReplacement (world, now);
            break;
        case SunStudyFollowState::UpdatingVisible:
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
    stats.lastError = gLastError;
    stats.description = gFollower.Describe ();
    stats.millisecondsUntilStart = gFollower.MillisecondsUntilStart (NowMs ());
    return stats;
}

} // namespace sunfollow
} // namespace geomsrv
