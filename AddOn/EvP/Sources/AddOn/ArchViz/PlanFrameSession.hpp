#ifndef EVP_ARCHVIZ_PLANFRAMESESSION_HPP
#define EVP_ARCHVIZ_PLANFRAMESESSION_HPP

// ArchViz/PlanFrameSession -- the main-thread half of the floor-plan frame record,
// a DIAGNOSTIC. Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md:
// it installs the Present hook, and it may only observe.
//
// WHAT IT ANSWERS. HANDOFF-OverlayPatch.md's floor-plan section leaves one
// decision to measurement: compose the plan overlay in our own window over the
// canvas (A), or into Archicad's plan frame at Present (B). B is exact by
// construction IF the transform each plan frame was drawn with is known at its
// Present -- and the plan has no camera on the GPU (frozen finding 13), so that
// transform can only come from ACAPI, on the main thread. The record says which
// chain the plan presents through and on which thread, inside which of the main
// thread's messages, and -- from the pixels -- which ACAPI read of the transform
// describes each frame: the one taken as the plan canvas's message began, the
// newest one published, a timer's, or only one taken after the frame.
//
// HOW. For a few seconds while the user pans and zooms the plan:
//   * the Present hook stamps every Present and keeps the plan's own frames
//     (ArchViz/Dxgi/PlanFrameRecord);
//   * a SUBCLASS of the plan canvas samples ACAPI_View_PointToCoord as each of its
//     messages begins and after it returns, and a 15 ms timer samples it too;
//   * a WH_GETMESSAGE hook records which message the main thread last retrieved.
// Then, on a worker thread, consecutive frames are registered
// (ArchViz/PlanFrameRegistration) and everything is written to
// logs\plan_frames\ -- raw, so the diagnostic's verdict can change without a
// rebuild and be replayed offline (§7).
//
// ⚠️ THE OVERLAY MUST BE OFF. The record measures Archicad's frames, and our own
// overlay's ACAPI poll, render thread and wake hook would be part of what it
// measures. Start refuses while any overlay session or camera-sync mode runs.
//
// ⚠️ EVERY START RESETS WHAT EVERY STOP LEAVES BEHIND (§8). The subclass, the
// message hook, the timer, the timer resolution, the Present hook and the
// experiment breadcrumb are all released on every exit: the end of the window,
// the plan leaving the front, the canvas being destroyed, a project closing, the
// add-on unloading.
//
// MAIN THREAD ONLY, except the analysis worker, which touches no ACAPI, no D3D
// and no window.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace planframes {

enum class SessionState : uint32_t { Idle = 0, Recording, Closing, Analysing, Done, Failed };
const char* SessionStateName (SessionState state);

struct SessionStatus {
    SessionState state = SessionState::Idle;
    std::string reason; // why the recording ended, or why it failed
    uint32_t seconds = 0;
    uint64_t elapsedMs = 0;
    // The Present half.
    uint64_t presents = 0;
    uint64_t presentsDropped = 0;
    uint64_t targetPresents = 0;
    uint64_t framesSubmitted = 0;
    uint64_t framesReady = 0;
    uint64_t slotsBusy = 0;
    uint64_t readbackFailures = 0;
    uint64_t createFailures = 0;
    uint64_t unsupportedFormat = 0;
    uint64_t targetChanges = 0;
    uint32_t format = 0;
    // The main-thread half.
    uint64_t samplesEntry = 0;
    uint64_t samplesExit = 0;
    uint64_t samplesTimer = 0;
    uint64_t samplesInvalid = 0;
    uint64_t samplesTorn = 0;
    uint64_t samplesDropped = 0;
    uint64_t canvasMessages = 0;
    uint64_t retrievedMessages = 0;
    uint32_t idleRedraws = 0; // still frames the session asked for, to anchor the analysis
    uint32_t mainThread = 0;
    double dpi = 0.0;
    std::string canvasClass;
    uint32_t canvasWidth = 0; // physical pixels
    uint32_t canvasHeight = 0;
    std::string target; // the plan's chain, hex; empty until identified
    std::string targetClass;
    std::string targetRelation;
    // The analysis.
    uint32_t pairs = 0;
    uint32_t pairsValid = 0;
    uint64_t analysisMs = 0;
    std::string jsonPath;
    std::string framesPath;
};

// MAIN THREAD. Refuses, with the reason, unless the floor plan is in front and no
// overlay, camera-sync mode or Present hook is running.
bool Start (uint32_t seconds, std::string& error);
// MAIN THREAD. End the recording early; what was recorded is still analysed.
void Stop (const char* reason);
// MAIN THREAD. Forget the last record and free its memory. Waits for an analysis
// in progress.
void Reset ();
// MAIN THREAD. A project closing or the add-on unloading: every hook out now, the
// worker joined, nothing analysed.
void Shutdown ();
SessionStatus GetStatus ();

} // namespace planframes
} // namespace archviz
} // namespace geomsrv

#endif
