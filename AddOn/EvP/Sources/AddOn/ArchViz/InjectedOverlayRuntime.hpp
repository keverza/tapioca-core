#ifndef EVP_ARCHVIZ_INJECTEDOVERLAYRUNTIME_HPP
#define EVP_ARCHVIZ_INJECTEDOVERLAYRUNTIME_HPP

// The overlay, as a service that stays alive (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 11).
//
// ⚠️ THIS IS THE PROMOTION, AND WHAT IT PROMOTES IS ALREADY PROVEN. Stages 5 to
// 10 established, by measurement, that Archicad's own uploaded GPU camera can be
// found, kept, and drawn with: the camera census identifies the draw family that
// carries it, the logical fingerprint survives Archicad rebuilding its views,
// `HostOccluders` answers "what is opaque" from the MODEL rather than from D3D
// blend state, and three overlay kinds compose against one depth buffer. None of
// that changes here. What changes is who drives it.
//
// ⚠️ THE DIAGNOSTIC IS NOT THE PRODUCT, AND THE DIFFERENCE IS CEREMONY. A
// regression command must show its working: orbit for twelve seconds, rank every
// candidate group, print the occurrence table, select by hand, prove the
// transform with probes. A user opening an overlay must be asked for none of it.
// `census::SetAutoSelect` is what removes it -- the census already scores every
// group on every model frame, so the moment one clears the gate there is nothing
// left to wait for.
//
// ⚠️ AND NOTHING IS TORN DOWN TO HIDE IT. `SetVisible (false)` stops the draw and
// keeps the camera lock, the host snapshot, the shaders and the buffers. Hiding
// an overlay is not a reason to re-learn a camera, re-walk a building and
// recompile eight shaders, and a runtime that did that would make its own
// re-showing slow enough to look broken.
//
// ⚠️ THE TWO SERVICES DO NOT WAIT ON EACH OTHER. Camera synchronisation and host
// extraction run independently: the overlay draws as soon as the camera locks,
// with self-occlusion only, and semantic host occlusion switches itself on when
// the snapshot publishes. A user should never watch a blank viewport while a
// building is walked -- run fifty-six spent 130 seconds doing exactly that.
//
// ⚠️ AND IT FAILS CLOSED. On any Archicad build the hook profile is not pinned
// to, `Start` refuses with a typed reason and the portable overlay remains the
// only renderer. The two are never run at once: `DiligentSceneDraw` drops its
// wireframe pass while this one is `Active`.
//
// THREAD. Every entry point here is MAIN THREAD. The work happens on Archicad's
// render thread inside the detours and on the extraction worker; this file only
// arms, disarms and reports.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {

// ⚠️ A TYPED REASON, BECAUSE `None` REACHED A USER. Run fifty-eight's first
// attempt ended with the line `Could not arm the GPU-state hooks: None` -- a
// caller read the wrong key off a failure envelope and printed the absence. A
// start result that can only be true or false invites exactly that.
enum class StartError : uint32_t {
    None = 0,
    // The 3D window has not drawn yet, so there is no context to hook. ⚠️ THIS
    // IS A NORMAL RUNTIME STATE AND NOT A FAILURE: the runtime keeps waiting and
    // arms itself when the viewport appears.
    WaitingFor3DContext,
    // ⚠️ THE FRONT WINDOW IS NOT ARCHICAD'S 3D MODEL, AND A FLOOR
    // PLAN IS NOT A SMALL 3D SCENE. This whole path finds the draw family that
    // carries Archicad's 3D MODEL camera and composes against its depth buffer;
    // a plan window has no such camera, no model depth and no perspective. The
    // portable overlay measures the plan's own zoom into a top-down orthographic
    // camera and draws there correctly, which is why it stays the renderer for
    // plans rather than being a lesser version of this one.
    Not3DWindow,
    BuildNotPinned, // fail closed: use the portable overlay
    HooksRefused,   // the mode would not accept the GPU-state slots
    DeviceUnsupported,
    AlreadyRunning,
    Internal,
};
const char* StartErrorName (StartError code);

struct StartResult {
    bool ok = false;
    StartError code = StartError::None;
    std::string message;
    // ⚠️ WHETHER WAITING WILL HELP, WHICH IS THE ONLY THING A CALLER CAN ACT ON.
    // `WaitingFor3DContext` is retryable and the runtime retries itself;
    // `BuildNotPinned` never becomes true and must fall back instead.
    bool retryable = false;
};

// What the camera is doing. `Reacquiring` is ORDINARY: a resize, a saved-view
// switch or a device reset all pass through it and back without user action.
enum class CameraState : uint32_t { Unavailable = 0, Learning, Locked, Reacquiring };
const char* CameraStateName (CameraState state);

// What the building is doing. `Dirty` means the model changed and the snapshot
// on the GPU is known to be behind it.
enum class HostState : uint32_t { Idle = 0, Extracting, Ready, Dirty };
const char* HostStateName (HostState state);

struct Health {
    bool running = false;
    bool visible = false;
    bool waitingForContext = false;
    CameraState camera = CameraState::Unavailable;
    HostState host = HostState::Idle;
    uint64_t autoSelections = 0;
    uint64_t reacquisitions = 0;
    uint32_t hostOpaqueTriangles = 0;
    uint64_t overlayDraws = 0;
    // Where the composition stopped, if it did. See `GetHealth`.
    uint64_t presentInjections = 0;
    uint64_t hostNoDepthTarget = 0;
    uint64_t hostNoGeometry = 0;
    uint64_t overlayNoEdges = 0;
    uint64_t overlayNoCamera = 0;
    uint64_t overlayCulled = 0;
    uint64_t skippedStaleCamera = 0;
    uint32_t linesDrawn = 0;

    // ⚠️ THE TWO RECTANGLES, BECAUSE THEIR DIFFERENCE IS THE
    // DESYNC. See `InjectionStats`: equal is healthy, different means the overlay
    // is projected into a window that no longer exists.
    uint32_t acceptedViewportWidth = 0, acceptedViewportHeight = 0;
    uint32_t sceneViewportWidth = 0, sceneViewportHeight = 0;

    // ⚠️ THE SURFACE BEING DRAWN INTO AND THE DEPTH BEING
    // TESTED AGAINST. D3D11 refuses a binding whose render target and depth view
    // differ in size, so unequal here means the composition did not land however
    // many lines it reported. See `overlaycompose::Stats::sizeMismatches`.
    uint32_t targetWidth = 0, targetHeight = 0;
    uint32_t composeDepthWidth = 0, composeDepthHeight = 0;
    uint64_t composeSizeMismatches = 0;
    uint64_t resizeRebinds = 0;
    uint32_t lastMissMask = 0;

    // ⚠️ THE CHAIN, IN ORDER, SO A FAILURE NAMES ITS OWN STAGE. Six
    // runs were spent asking "why is nothing on screen" when the answer was a
    // different stage each time and no single number distinguished them.
    uint64_t modelFramesSeen = 0;
    uint32_t eligibleCandidates = 0;
    uint64_t selectionAttempts = 0;
    // ⚠️ WHETHER DRAWS REACH THE TABLE AT ALL, which no previous
    // chain could say. A gate cannot refuse a candidate that was never scored.
    uint64_t drawsSeen = 0;
    uint64_t drawsQualified = 0;
    uint32_t groupsUsed = 0;
    uint64_t groupsOverflowed = 0;
    bool selectionValid = false;
    uint32_t selectedGroup = 0;
    uint32_t selectedOccurrence = 0;
    bool occurrenceLocked = false;
    std::string cameraSource;
    std::string armState;
    uint64_t logicalMatches = 0;
    uint64_t authoritativeSnapshots = 0;
    uint64_t presentsSeen = 0;

    // ⚠️ THE FIRST STAGE THAT IS NOT SATISFIED, COMPUTED IN ONE PLACE
    // FROM THE FIELDS ABOVE. "BLOCKED AT Selection" ends an investigation that
    // otherwise costs a run per hypothesis.
    std::string blockedAt;
    StartError lastError = StartError::None;
    std::string lastMessage;
};

// MAIN THREAD. Idempotent: starting a running runtime succeeds and changes
// nothing. Returns immediately -- the camera is learned and the building walked
// in the background, and `GetHealth` reports how far each has got.
StartResult Start ();

// MAIN THREAD. Show or hide the overlay WITHOUT destroying anything. See the
// header note.
void SetVisible (bool visible);
bool Visible ();
bool Running ();

// MAIN THREAD, periodic and cheap. Retries what `Start` could not do yet --
// principally arming once Archicad's 3D context appears -- and keeps the health
// record current. Safe to call when nothing is running.
void Tick ();

Health GetHealth ();

// MAIN THREAD. Release everything: hooks, census, shaders, buffers, snapshot.
// Only for shutdown or a deliberate reset; hiding uses `SetVisible`.
void Stop ();

} // namespace overlayruntime
} // namespace archviz
} // namespace geomsrv

#endif
