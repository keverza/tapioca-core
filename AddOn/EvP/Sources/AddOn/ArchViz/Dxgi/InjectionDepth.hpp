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

// MAIN THREAD.
void SetMode (Mode mode);
Mode GetMode ();

// RENDER THREAD, from the scene pass: the depth view the model was drawn with.
// ⚠️ A REFERENCE, RELEASED WHEN REPLACED AND AT TEARDOWN. Holding a view of a
// resource Archicad may resize is the mistake the back-buffer path refuses; the
// private copy is rebuilt whenever the source description changes.
void RetainSceneView (ID3D11DepthStencilView* view);

// RENDER THREAD, immediately before an injected draw. Returns the view to bind,
// or null when the mode is `Off` or nothing is available yet. For `PrivateCopy`
// this is where Archicad's depth is copied into ours.
ID3D11DepthStencilView* PrepareForInjection (ID3D11DeviceContext* context);

// The state to pair with whatever `PrepareForInjection` returned: writes are off
// for `SceneReadOnly` and on for `PrivateCopy`, and that is not a preference.
ID3D11DepthStencilState* StateForMode (ID3D11DeviceContext* context);

// MAIN THREAD, at teardown.
void Shutdown ();

struct Stats {
    uint64_t preparations = 0;
    uint64_t copies = 0;
    uint64_t noSceneView = 0;
    uint64_t copyRefused = 0;      // the source could not be copied from
    uint64_t rebuilds = 0;         // the source description changed
    uint32_t width = 0, height = 0;
    uint32_t format = 0;
    uint32_t sampleCount = 0;
    bool     privateReady = false;
    char     lastError[160] = {};
};
Stats GetStats ();

}   // namespace depth
}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
