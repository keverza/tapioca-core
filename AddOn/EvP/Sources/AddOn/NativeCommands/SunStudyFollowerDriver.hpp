#ifndef EVP_NATIVECOMMANDS_SUNSTUDYFOLLOWERDRIVER_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYFOLLOWERDRIVER_HPP

// NativeCommands/SunStudyFollowerDriver — the live orchestration that makes a
// sun study follow the model.
//
// ⚠️ IT DECIDES NOTHING. Every rule about when a study is stale, how long the
// quiet period is and which result may become visible lives in the pure
// `SunStudy/SunStudyFollower`, which is tested offline over a supplied clock
// because those rules are about TIME and IDENTITY and a live session cannot be
// asked to reproduce either on demand. This file only:
//
//     derives the current dependency signature
//     feeds follower.Observe
//     executes the follower's decisions
//     drives the EXISTING commands
//     reports diagnostics
//
// ⚠️ AND IT IMPLEMENTS NO ANALYSIS. It is an internal command client:
// Snapshot/display use ExecuteNativeCommand. Preparation uses StartSunStudy's
// shared capture/preparation seam, with the pure portion on a task worker. Slices
// use SunStudyAdvanceWorker over the SAME SunStudyStore::Advance as the public
// advance command. There must stay ONE implementation of a sun study,
// and a driver that copied StartSunStudy's sun gather, sampling, winding proof
// and atlas build would be a second one -- silently diverging the first time
// either was fixed.
//
// The division, top to bottom:
//
//     SunStudyFollower        pure policy and state
//     SunStudyFollowerDriver  this file: live orchestration
//     ExecuteNativeCommand    the existing implementations
//     SunStudySession         the analysis
//     DiligentScene           a display consumer, and nothing more

#include "SunStudy/SunStudyFollower.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace sunfollow {

// What a manually started study was run with, so a replacement can be run the
// same way.
//
// ⚠️ CAPTURED FROM A STUDY THAT SUCCEEDED, NEVER INVENTED. Opening the viewer
// must not spontaneously start analysing a building nobody asked about: until a
// person has run one study and seen it, there is no configuration and the
// follower sits in `NoStudy`.
struct ActiveSunStudyConfig {
    int year = 0, month = 0, day = 0;
    int timestep = 60;
    int hourFrom = 0, hourTo = 24;
    double minAltitudeDeg = 0.0;
    double grid = 2.0;
    // ⚠️ THE DOMAIN IS PART OF WHAT WAS MEASURED, NOT A DISPLAY PREFERENCE.
    // Without it every rerun was a TRIANGLE study: the first edit after a patch
    // study silently swapped the picture back to per-triangle tiles, seams
    // along every diagonal, while the smoke that started it reported PASS.
    bool patchDomain = false;
    // The ROLES, as picked (SunStudy/SunStudyRoles.hpp). Measurement inputs for
    // the same reason the domain is: a rerun without them analyses the context.
    std::vector<std::string> analysisElements;
    std::vector<std::string> contextElements;
    std::vector<std::string> ignoredElements;
    // Display, carried so a rerun comes back looking the way the user left it --
    // a replacement that reverted to the hours ramp while they were reading a
    // `cell checker` would read as the diagnostic having been taken away.
    uint32_t debug = 0;
    uint32_t depth = 0;
    double hoursMax = 0.0; // 0 = let ShowSunStudy derive it
    bool valid = false;
};

struct FollowerStats {
    evp::sunstudy::SunStudyFollowState state = evp::sunstudy::SunStudyFollowState::NoStudy;
    evp::sunstudy::SunStudyDirtyReason dirtyReason = evp::sunstudy::SunStudyDirtyReason::None;
    bool autoFollow = false;
    bool dirty = false;
    uint64_t generation = 0;
    std::string studyId;
    uint64_t studySnapshot = 0;
    uint64_t sceneSnapshot = 0;
    int64_t millisecondsUntilStart = -1; // -1 when nothing is waiting
    uint64_t starts = 0;
    uint64_t acceptedCompletions = 0;
    uint64_t discardedCompletions = 0;
    uint64_t automaticReruns = 0;
    uint64_t snapshotRebuilds = 0;
    uint64_t sessionGeneration = 0;
    uint64_t cancelledRuns = 0;
    uint64_t sliceSubmissions = 0;
    uint64_t sliceCompletions = 0;
    size_t resolvedSteps = 0;
    size_t totalSteps = 0;
    bool workerBusy = false;
    std::string stage;
    bool navigationDeferred = false;
    std::string tickThread;
    std::string workerThread;
    std::string lastError;
    std::string description;
};

// Adopt the study `studyId` as the one to follow, with the configuration it was
// run with. Called by `ShowSunStudy` after a MANUAL display succeeded. MAIN
// THREAD via the gate if needed. A display begun before close cannot re-arm it.
void Adopt (const std::string& studyId, const ActiveSunStudyConfig& config, uint64_t sessionGeneration);
uint64_t SessionGeneration ();

// Disarm and cancel only the driver's replacement study, without joining the
// worker or erasing the manually displayed cache. Advances drain cooperatively.
void Disable ();

// Quit/unload only: disarm, then join before the module can be unloaded.
void Shutdown ();

// One scheduling step. MAIN THREAD ONLY -- it calls ACAPI through the commands
// it drives.
//
// Poll/submit preparation and calculation without waiting. Geometry/sun capture
// and display assembly still require main-thread time. Known navigation defers
// host capture, new slices and display; already-running pure work drains off-thread.
void Tick ();

FollowerStats State ();

} // namespace sunfollow
} // namespace geomsrv

#endif
