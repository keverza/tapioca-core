#ifndef EVP_ARCHVIZ_DXGI_INJECTIONRENDERER_HPP
#define EVP_ARCHVIZ_DXGI_INJECTIONRENDERER_HPP

// Stage 5, Proof A: draw ONE world-space primitive inside Archicad's own scene
// pass, with Archicad's own camera (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ THIS IS THE EXPERIMENT THE WHOLE RUNG EXISTS FOR, AND IT IS DELIBERATELY
// TINY. One hard-coded triangle, its own vertex and pixel shader, its own vertex
// buffer, on ARCHICAD'S device and ARCHICAD'S context, bound to ARCHICAD'S
// camera constant-buffer windows. No Diligent, no shared texture, no ACAPI
// camera, no prediction, no reprojection, and no matrix ever copied to the CPU.
// If this stays pixel-locked through a fast orbit, camera synchronisation is
// finished and everything after it is renderer engineering.
//
// ⚠️ WHY A COMPOSITE AT PRESENT COULD NEVER HAVE DONE THIS. The overlay's old
// path renders on its own device and is composited after Archicad's frame is
// finished, so a fresh camera can only be applied by reprojecting a finished
// image. Rotation about the eye reprojects; arbitrary translation does not --
// parallax is missing information, not misplaced information -- and occlusion
// against Archicad's geometry is impossible from a flat composite. Drawing
// inside the pass removes both limits at once.
//
// ⚠️ THE VERTEX SHADER READS ARCHICAD'S GPU BYTES DIRECTLY. `b1` is its view and
// `b2` its projection (stage 3, run nineteen: their product scores 0.128 px),
// bound through `VSSetConstantBuffers1` with the exact buffer, firstConstant and
// numConstants the final scene draw consumed. Nothing is copied, so there is no
// readback, no staleness and no synchronisation with the main thread.
//
// ⚠️ EVERY ENTRY POINT RUNS ON ARCHICAD'S RENDER THREAD, INSIDE A DETOUR, AND
// UNDER `ScopedInjectionGuard`. Without the guard our own D3D calls would be
// recorded as Archicad's -- they arrive at the same detours on the same context
// object and no pointer filter can separate them.

#include "ArchViz/Dxgi/InjectionCamera.hpp"

#include <cstdint>

struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

// ⚠️ ARMING IS NOT HAVING A CAMERA, AND RUN FORTY-FIVE FAILED ON
// EXACTLY THAT CONFUSION. The caller asked "is the selected camera valid?" as a
// PRECONDITION of enabling -- but the camera only becomes valid when a draw
// matching the selection is snapshotted, and no such draw can happen while the
// injection is disabled. Enabling required the thing enabling produces.
//
// Three states break the circle:
//
//   Disabled            nothing is drawn and nothing is snapshotted.
//
//   ArmedPendingCamera  the fingerprint is known and we are waiting for a draw
//                       that matches it. ⚠️ SNAPSHOTS HAPPEN IN THIS
//                       STATE AND DRAWS DO NOT -- that is the whole point of it
//                       existing. There is no camera yet, so there is nothing to
//                       draw with, but the machinery that FINDS one is live.
//
//   Active              a matching draw has been snapshotted. From here the
//                       primitive is drawn with that camera.
//
// The transition out of `ArmedPendingCamera` is made by the first matching draw,
// on the render thread, and by nothing else. A precondition that cannot be met
// before arming is not a safety check, it is a deadlock.
enum class ArmState : uint32_t { Disabled = 0, ArmedPendingCamera = 1, Active = 2 };

// MAIN THREAD. Off by default; `Tapioca.ViewerInjectTriangle` arms it.
// `SetEnabled (true)` moves Disabled -> ArmedPendingCamera and never further:
// only a matching draw can reach `Active`.
void SetEnabled (bool enabled);

// True while anything is armed at all -- `ArmedPendingCamera` OR `Active`. The
// snapshot path tests this, because it is what has to run BEFORE there is a
// camera; the draw paths test `Active`.
bool Enabled ();
ArmState GetArmState ();

// RENDER THREAD, from `InjectionCamera` when a snapshot first becomes valid.
// Idempotent, and it can never arm anything that was not already armed.
void NotifyCameraAcquired ();

// MAIN THREAD. Where in the model to put the test primitive, in world metres.
//
// ⚠️ A TRIANGLE AT THE ORIGIN ONLY TESTS ANYTHING IF THE MODEL HAS SOMETHING
// RECOGNISABLE THERE. The sharpest version of this proof puts a vertex on a
// known Archicad point or edge: a one-pixel error is then obvious, where a large
// primitive floating in space can drift several pixels and still look attached.
// Small and anchored beats big and free.
void SetAnchor (float x, float y, float z, float sizeMetres);

// MAIN THREAD. Release every device object. Called at teardown, and before a
// resize, because nothing here may outlive Archicad's device.
void Shutdown ();

// RENDER THREAD, from the detour that recognises scene completion, BEFORE that
// call is forwarded -- so Archicad's scene render target and depth buffer are
// still the bound targets and its viewport is still set.
//
// Does nothing unless: the mode is armed, the last verified scene draw is valid,
// and its `b1` and `b2` bindings both came from that same draw. A frame it
// cannot vouch for is a frame it skips; a skipped frame is invisible and a
// wrongly-drawn one is the bug.
void InjectIfReady (ID3D11DeviceContext* context);

// ⚠️ THE CAMERA ITSELF IS `InjectionCamera`, INCLUDED ABOVE, AND ITS DECLARATIONS
// ARE NOT REPEATED HERE. `SnapshotCamera`, `SnapshotSelectedDraw`,
// `CameraSource`, `SelectedCameraState` and the interpretation agreement all
// live there, because what the primitive is drawn WITH and how it is DRAWN are
// two questions -- and every failure from run twenty-eight onward was in the
// first while the second was never in doubt.

// RENDER THREAD, from the present detour, BEFORE the present is forwarded.
//
// ⚠️ THIS IS PROOF A's INJECTION POINT, AND IT IS DELIBERATELY NOT THE FINAL
// ONE. Run twenty-four injected 491 times out of 491 scene departures and the
// triangle was visible only when navigation STOPPED -- because Archicad's model
// pass is four draws wide and there are several of them, so injecting at the
// first one puts the triangle underneath everything drawn afterwards. At rest
// Archicad redraws little and it survived; in motion it was painted over every
// frame. That is failure B, overwritten later, and not a camera problem.
//
// Injecting immediately before Present puts it on top of everything, which
// settles the only question Proof A asks: does a world-space primitive
// transformed by Archicad's own GPU camera stay welded to the model through fast
// navigation? It gives up depth occlusion, which Proof B needs and which is why
// the in-pass path above stays.
//
// The back-buffer view is created from the swap chain here and released
// immediately -- nothing is cached, so `ResizeBuffers` stays possible.
void InjectAtPresent (ID3D11DeviceContext* context, IDXGISwapChain* swapChain, uint64_t modelSceneGeneration);

// Which injection point is armed. Proof A uses Present; Proof B will use the
// scene pass, where Archicad's depth buffer still exists.
// ⚠️ `Both` DRAWS AT BOTH POINTS IN ONE RUN, which is the only way to tell three
// possible failures apart without three runs and three memories of what the
// screen looked like. The scene-pass triangle is drawn inside Archicad's own
// pass, where its depth buffer still exists and later geometry can paint over
// it; the Present triangle is drawn on top of the finished frame. Seeing one and
// not the other is itself the answer.
enum class Point { ScenePass, Present, Both };
void SetPoint (Point point);

// MAIN THREAD. The stage-5 proof primitives: the magenta anchor triangle, the
// clip-space wedge and the FRONT/BEHIND depth probes.
//
// ⚠️ THESE ARE INSTRUMENTS AND A USER MUST NEVER SEE THEM. They
// exist to answer questions a picture cannot -- does the projection put the
// anchor where the oracle says, does a primitive in front of the model survive
// the depth test, does the output path rasterise at all -- and every one of those
// questions is settled. They were drawn UNCONDITIONALLY, so promoting the
// overlay to a menu item put a magenta triangle and two coloured wedges over the
// user's model.
//
// Off by default, because production is the default. The diagnostic turns them
// on for the runs that still need them.
// MAIN THREAD, from `InjectedOverlayRuntime::Arm`, before anything is enabled.
// Zero every counter this session will report and forget the accepted camera.
// ⚠️ SEE `injection::ResetCameraCounters` FOR WHAT THIS COST.
void BeginSession ();

void SetProofPrimitives (bool enabled);
bool ProofPrimitives ();
Point GetPoint ();

struct InjectionStats {
    uint64_t injected = 0;
    uint64_t skippedNoSceneDraw = 0;
    uint64_t skippedNoCamera = 0; // b1 or b2 missing from that draw
    uint64_t skippedNotReady = 0; // device objects could not be created
    // ⚠️ THE PREDICATE FAILS CLOSED AND EACH REFUSAL IS NAMED. A frame that
    // cannot be vouched for is skipped; a skipped frame is invisible and a
    // wrongly-drawn one is the bug.
    uint64_t skippedPassMismatch = 0; // the latched draw is not this pass/epoch
    uint64_t skippedWindowSize = 0;   // b1/b2 are not the expected 256-byte windows
    uint64_t skippedReentrant = 0;    // we were already inside an injection
    // ⚠️ THE CAMERA WAS LATCHED IN AN EARLIER FRAME. Drawing with it would be
    // geometrically valid and one frame wrong, which is the exact fault this
    // rung exists to remove and the one no pixel test at rest can see.
    uint64_t skippedStaleCamera = 0;
    uint64_t backBufferFailures = 0;

    // ⚠️ THE TWO RECTANGLES WHOSE DIFFERENCE IS THE DESYNC.
    // `accepted*` is the viewport the camera Present draws with was measured in;
    // `liveScene*` is the viewport Archicad drew the model in on the most recent
    // qualifying draw. Equal is healthy. Different means the overlay is being
    // projected into a window that no longer exists, which is what a resize looks
    // like from the user's side: geometry frozen somewhere unrelated. Packed as
    // width << 16 | height and published by Present, so reading them costs one
    // atomic load and no lock.
    uint32_t acceptedViewportWidth = 0, acceptedViewportHeight = 0;
    uint32_t liveSceneViewportWidth = 0, liveSceneViewportHeight = 0;

    // The two injection points, counted apart: one fires per scene departure and
    // the other per Present, so a shared counter is a meaningless ratio.
    uint64_t injectedPresent = 0;
    uint64_t injectedScenePass = 0;

    // ⚠️ THE RATIO THAT MATTERS: `occurrenceDraws / occurrenceModelFrames` is how
    // many times the selected group draws per model frame -- six, in run
    // thirty-four -- and `authoritativeSnapshots` must be ONE per model frame,
    // not six, once an occurrence is locked.
    uint64_t occurrenceDraws = 0;
    uint64_t authoritativeSnapshots = 0;
    uint64_t occurrenceModelFrames = 0;
    bool occurrenceLocked = false;
    uint32_t lockedOccurrence = 0;

    uint32_t shaderInterpretation = 0;
    uint32_t expectedInterpretation = 0;
    bool interpretationAgrees = false;
    uint64_t skippedInterpretation = 0;

    // ⚠️ THE THREE FRAME STATES, WHICH SAY MORE THAN ANY SKIP COUNT. Healthy is
    // newScene + repeatScene == Presents, with invalidScene at zero.
    //   NEW_SCENE     the model was re-rendered and brought its own camera
    //   REPEAT_SCENE  no new model scene, so the accepted camera still describes
    //                 what is on screen -- draw again into this back buffer
    //   INVALID_SCENE the model moved on without a camera: forbidden, and the
    //                 only state worth skipping a frame for
    uint64_t newScene = 0;
    uint64_t repeatScene = 0;
    uint64_t invalidScene = 0;

    // ⚠️ ONE GENERIC `INVALID` COUNTER HID THE NEXT PROBLEM FOR FOUR RUNS. These
    // four reasons are different faults with different fixes, and lumping them
    // together turned a specific, answerable question into a shrug.
    uint64_t invalidNoSnapshot = 0;           // nothing has been snapshotted at all
    uint64_t invalidNoDrawThisGeneration = 0; // the candidate did not draw this frame
    uint64_t invalidGenerationAdvanced = 0;   // the model moved on before we drew
    uint64_t invalidGenerationMismatch = 0;   // snapshot and Present disagree

    // ⚠️ THE SNAPSHOT ARITHMETIC, AND ITS INVARIANT IS AN EQUALITY.
    //
    //     qualifyingCameraDraws == viewCopies == projectionCopies
    //
    // Any inequality names its own cause: fewer copies than qualifying draws
    // means the window predicate refused, and a difference BETWEEN the two copy
    // counts would mean one of the two `CopySubresourceRegion` calls is not
    // happening at all -- which no picture could ever show.
    uint64_t qualifyingCameraDraws = 0;

    // ⚠️ THE SELECTED GROUP'S OWN ARITHMETIC, KEPT APART FROM THE LEARNER'S. In
    // phase B these must satisfy
    //
    //     selectedGroupDraws == selectedGroupSnapshots == snapshotsTaken
    //
    // and `snapshotsTaken == 0` beside `selectedGroupDraws > 0` is a HARNESS
    // failure -- the camera is not connected to the injection -- not a finding
    // about cameras or rasterisation.
    uint64_t selectedGroupDraws = 0;
    uint64_t selectedGroupSnapshots = 0;
    uint32_t selectedGroupId = 0;
    uint64_t selectedSnapshotGeneration = 0;
    uint64_t viewCopies = 0;
    uint64_t projectionCopies = 0;
    uint64_t snapshotsTaken = 0;   // generations completed: both halves copied
    uint64_t snapshotSequence = 0; // the sequence number of the newest snapshot
    bool snapshotValid = false;
    bool initialised = false;
    char lastError[192] = {};
};
InjectionStats GetInjectionStats ();

} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
