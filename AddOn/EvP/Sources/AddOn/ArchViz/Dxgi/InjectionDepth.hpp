#ifndef EVP_ARCHVIZ_DXGI_INJECTIONDEPTH_HPP
#define EVP_ARCHVIZ_DXGI_INJECTIONDEPTH_HPP

// The depth buffer an injected draw tests against (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 6).
//
// ⚠️ PROOFS A AND B ARE PASSED AND THIS FILE SPENDS THEM. Proof B established
// that a Tapioca primitive reads Archicad's own depth buffer correctly -- front
// geometry survives, geometry behind the model is rejected on 99.8% of frames,
// with depth written never. Proof B2 established the part that makes it usable:
// that same depth view is **still valid at Present**, where the overlay is
// always visible, so the overlay no longer has to choose between depth and
// persistence.
//
// Three modes, and the difference between the last two is the whole design
// question for ghost geometry:
//
//   Off             no depth at all. The Proof A control: always visible,
//                   never occluded, and primitives paint in draw order.
//
//   SceneReadOnly   Archicad's own depth view, test ON, ⚠️ WRITES OFF. The
//                   building hides what is behind it and we cannot change a
//                   pixel of Archicad's buffer. But our own primitives still do
//                   not occlude EACH OTHER, because nothing writes depth.
//
//   PrivateCopy     Archicad's depth resource is copied into a texture we own,
//                   once per injected frame, and we render into ours with test
//                   AND write on. The building still hides what is behind it --
//                   the copy carries its depth -- and ghost surfaces now occlude
//                   each other correctly.
//
// ⚠️ THE PRIVATE COPY IS WHAT MAKES SELF-OCCLUSION POSSIBLE WITHOUT TOUCHING
// ARCHICAD'S BUFFER, and that distinction is the reason it exists. Enabling
// depth writes against Archicad's own view would give the same picture and would
// leave our geometry in the depth buffer of a frame Archicad is still composing
// -- a change to somebody else's render that no experiment here has earned the
// right to make.
//
// THREAD SAFETY. `RetainSceneView` and `PrepareForInjection` run on Archicad's
// render thread inside a detour, under `ScopedInjectionGuard`. `SetMode` and
// `Shutdown` are main thread; nothing here may outlive Archicad's device.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11DepthStencilState;
struct ID3D11DepthStencilView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace depth {

enum class Mode : uint32_t { Off = 0, SceneReadOnly = 1, PrivateCopy = 2 };

// ⚠️ WHEN THE COPY IS TAKEN, WHICH RUN FORTY-EIGHT PROVED MATTERS
// MORE THAN WHETHER IT IS TAKEN. That run copied Archicad's depth at Present and
// the numbers came back wrong in a way no camera fault could explain:
//
//     PROOF B    FRONT 231/235 off  ->  173/235 on   26% of FRONT rejected
//                BEHIND 232/235 off ->  102/235 on
//
// Geometry in FRONT of the model was being occluded. Run forty-six, sampling the
// same buffer earlier, rejected 0% of FRONT. Something writes depth NEARER than
// the model between the two moments -- and the user sees it as a transparent
// build plane blocking the overlay completely, and as ghost surfaces cut by
// rectangles that are nothing of ours.
//
// A transparent plane has no business writing depth that occludes anything, but
// it does, and by Present that write is in the buffer. So the question is not
// "which buffer" but "which INSTANT of it":
//
//   AfterOpaque       the moment the last depth-writing, unblended draw of the
//                     model finished. ⚠️ THE ONE THIS IS LOOKING FOR:
//                     walls occlude, glass and helpers do not.
//
//   AfterTransparent  the end of the scene pass, after blended geometry. What
//                     `SceneReadOnly` has always bound.
//
//   AtPresent         what run forty-eight used, and the contaminated one.
//
// The three are swept and MEASURED against the FRONT/BEHIND probe pair rather
// than chosen by eye, because "the overlay looks right" is what six earlier runs
// thought too.
enum class Source : uint32_t { AfterOpaque = 0, AfterTransparent = 1, AtPresent = 2 };
void SetSource (Source source);
Source GetSource ();

// MAIN THREAD.
void SetMode (Mode mode);
Mode GetMode ();

// RENDER THREAD, from the scene pass: the depth view the model was drawn with.
// ⚠️ A REFERENCE, RELEASED WHEN REPLACED AND AT TEARDOWN. Holding a view of a
// resource Archicad may resize is the mistake the back-buffer path refuses; the
// private copy is rebuilt whenever the source description changes.
void RetainSceneView (ID3D11DepthStencilView* view);

// ANY THREAD. The retained view, so other diagnostics can tell a draw into the
// model's depth buffer from a draw into somebody else's.
ID3D11DepthStencilView* SceneView ();

// ⚠️ CLASSIFY EVERY DRAW INTO THE SCENE DEPTH VIEW, BEFORE IT IS
// FORWARDED. A draw is OPAQUE when blending is off and it writes depth; anything
// else -- blended, or depth-write disabled -- is what contaminates the buffer
// for our purposes. The classification is asked of the context with `OMGet*`
// rather than tracked through new hooked slots: adding a slot would change the
// patch profile and force every pinned build to be re-pinned, which is a high
// price for a fact two COM calls already answer.
//
// The `AfterOpaque` snapshot is taken at the FIRST non-opaque draw that follows
// an opaque one, which is the boundary this is looking for.
void OnDraw (ID3D11DeviceContext* context, uint64_t boundDepthStencil, uint64_t modelGeneration);

// RENDER THREAD, at the scene-pass departure. Takes the `AfterTransparent`
// snapshot.
void OnScenePassEnd (ID3D11DeviceContext* context, uint64_t modelGeneration);

// ⚠️ WHAT THE DEPTH BUFFER HAD IN IT AND WHO PUT IT THERE. Without
// this, "the overlay is occluded by nothing visible" is a shrug; with it, the
// draw that wrote the offending depth is counted and its kind is named.
struct Provenance {
    uint64_t sceneDraws = 0;
    uint64_t opaqueDraws = 0;
    uint64_t blendedDraws = 0;
    uint64_t noDepthWriteDraws = 0;
    uint64_t depthTestOffDraws = 0;

    // ⚠️ OPAQUE GEOMETRY AFTER THE TRANSITION IS THE ASSUMPTION THIS
    // EXISTS TO FALSIFY. `AfterOpaque` takes the FIRST opaque-to-blended
    // boundary; if Archicad goes back to opaque geometry afterwards, the
    // snapshot is early and this number says so rather than the picture.
    uint64_t opaqueAfterTransition = 0;
    uint64_t transitions = 0;
    uint64_t framesWithoutOpaque = 0;
    uint64_t framesWithoutTransition = 0;

    uint64_t capturedAfterOpaque = 0;
    uint64_t capturedAfterTransparent = 0;
    uint64_t capturedAtPresent = 0;
    uint64_t servedStale = 0; // asked for a snapshot this frame never took

    uint32_t lastOpaqueDrawIndex = 0;
    uint32_t lastSceneDrawIndex = 0;
};
Provenance GetProvenance ();
void ResetProvenance ();

// RENDER THREAD, immediately before an injected draw. Returns the view to bind,
// or null when the mode is `Off` or nothing is available yet. For `PrivateCopy`
// this is where Archicad's depth is copied into ours.
ID3D11DepthStencilView* PrepareForInjection (ID3D11DeviceContext* context);

// The state to pair with whatever `PrepareForInjection` returned: writes are off
// for `SceneReadOnly` and on for `PrivateCopy`, and that is not a preference.
ID3D11DepthStencilState* StateForMode (ID3D11DeviceContext* context);

// MAIN THREAD, at teardown.
void Shutdown ();

// ⚠️ EVERY STAGE OF THE PRIVATE COPY IS COUNTED SEPARATELY, so a failure names
// the step that failed instead of producing an absence. "It did not work" is not
// a diagnosis, and a path with seven ways to fail needs seven counters.
struct Stats {
    uint64_t preparations = 0;
    uint64_t sourceViewAcquired = 0;
    uint64_t sourceResourceAcquired = 0;
    uint64_t sourceDescAccepted = 0;
    uint64_t textureCreated = 0;
    uint64_t viewCreated = 0;
    uint64_t copyIssued = 0;
    uint64_t viewBound = 0;
    uint64_t copies = 0;
    uint64_t noSceneView = 0;
    uint64_t copyRefused = 0; // the source could not be copied from
    uint64_t rebuilds = 0;    // the source description changed
    uint32_t width = 0, height = 0;
    uint32_t format = 0;
    uint32_t sampleCount = 0;
    bool privateReady = false;
    char lastError[160] = {};
};
Stats GetStats ();

} // namespace depth
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
