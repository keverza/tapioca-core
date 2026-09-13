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

#include <cstdint>

struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

// MAIN THREAD. Off by default; `Tapioca.ViewerInjectTriangle` arms it.
void SetEnabled (bool enabled);
bool Enabled ();

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

// RENDER THREAD, from a draw detour, for a qualifying camera-bearing draw of the
// learned model pass.
//
// ⚠️ THE BINDING IS NOT THE BYTES, AND THAT DISTINCTION IS THIS FUNCTION.
// `buffer + firstConstant + numConstants` identifies WHERE the camera lived when
// the model was drawn; it does not preserve WHAT was there. Archicad's constants
// live in one 8 MiB ring, and between the model draw and Present its later
// passes keep writing that ring -- so rebinding the same window at Present can
// hand the shader another pass's bytes wearing the camera's address.
//
// That fits the symptom exactly. During navigation there is a great deal of
// later ring traffic and the world triangle vanished; when navigation stopped
// there was almost none, the window still held the model camera, and it
// appeared. The clip-space probe was rock solid throughout, which had already
// ruled out the injection, the back buffer and the state restoration.
//
// So the 256 bytes are copied GPU-to-GPU into a buffer we own, at the moment the
// draw that consumes them happens. ⚠️ NO `Map`, NO CPU READBACK, NO
// SYNCHRONISATION -- one `CopySubresourceRegion`, which for buffer resources
// takes BYTE coordinates rather than texels.
void SnapshotCamera (ID3D11DeviceContext* context);

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
void InjectAtPresent (ID3D11DeviceContext* context, IDXGISwapChain* swapChain,
                      uint64_t modelSceneGeneration);

// Which injection point is armed. Proof A uses Present; Proof B will use the
// scene pass, where Archicad's depth buffer still exists.
enum class Point { ScenePass, Present };
void  SetPoint (Point point);
Point GetPoint ();

struct InjectionStats {
    uint64_t injected = 0;
    uint64_t skippedNoSceneDraw = 0;
    uint64_t skippedNoCamera = 0;    // b1 or b2 missing from that draw
    uint64_t skippedNotReady = 0;    // device objects could not be created
    // ⚠️ THE PREDICATE FAILS CLOSED AND EACH REFUSAL IS NAMED. A frame that
    // cannot be vouched for is skipped; a skipped frame is invisible and a
    // wrongly-drawn one is the bug.
    uint64_t skippedPassMismatch = 0;   // the latched draw is not this pass/epoch
    uint64_t skippedWindowSize = 0;     // b1/b2 are not the expected 256-byte windows
    uint64_t skippedReentrant = 0;      // we were already inside an injection
    // ⚠️ THE CAMERA WAS LATCHED IN AN EARLIER FRAME. Drawing with it would be
    // geometrically valid and one frame wrong, which is the exact fault this
    // rung exists to remove and the one no pixel test at rest can see.
    uint64_t skippedStaleCamera = 0;
    uint64_t backBufferFailures = 0;

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

    // How many times the camera bytes have been copied out of Archicad's ring.
    uint64_t snapshotsTaken = 0;
    bool     snapshotValid = false;
    bool     initialised = false;
    char     lastError[192] = {};
};
InjectionStats GetInjectionStats ();

}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
