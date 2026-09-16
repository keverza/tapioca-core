#ifndef EVP_ARCHVIZ_DXGI_DEPTHCHECKPOINTS_HPP
#define EVP_ARCHVIZ_DXGI_DEPTHCHECKPOINTS_HPP

// WHEN, within one Archicad frame, its depth buffer still holds only the model
// (PLAT-RE155, docs/architecture/api/HANDOFF-OverlayPatch.md stage 8).
//
// ⚠️ THIS EXISTS BECAUSE INFERRING THE ANSWER FROM D3D STATE FAILED OUTRIGHT.
// Run forty-nine classified every draw into the scene depth view by blend and
// depth-write state, looking for the opaque-to-transparent boundary, and
// measured:
//
//     scene draws into it              : 2914
//     opaque (unblended, depth-writing): 0
//     blended                          : 2914
//     opaque-to-blended boundaries     : 0
//
// **Every** draw has `BlendEnable = TRUE`. Archicad binds one blend state and
// expresses opacity through the equation -- `ONE`/`ZERO` blends to exactly the
// source -- not through the enable bit. So there is no boundary to find in the
// state, the `AfterOpaque` snapshot never fired, and 519 injections were refused
// as "the moment never arrived", which was true and useless.
//
// ⚠️ SO THE BOUNDARY IS MEASURED, NOT INFERRED. Depth is copied after EVERY draw
// into the scene depth view, and each copy is then tested at Present with the
// same FRONT and BEHIND primitives the depth proof already uses. The checkpoint
// where FRONT still survives and BEHIND is rejected is the model's own depth,
// whatever D3D state produced it; the next draw is the contaminator.
//
// The draw's attributes -- ordinal, kind, index count, topology, shader and
// state identities -- are RECORDED BESIDE EACH CHECKPOINT AND NOT USED TO
// CHOOSE IT. They are there to explain the transition after the probes have
// found it, which is the opposite of what run forty-nine did.
//
// ⚠️ SPARSE, BECAUSE THESE ARE FULL-RESOLUTION DEPTH COPIES. One model frame in
// `interval` is instrumented; the rest are untouched. Eight checkpoints at
// 3432x1803 is roughly 200 MB of VRAM and eight whole-resource copies on the
// frames that are instrumented, which is affordable for a diagnostic and would
// not be for production. Nothing here ships enabled.
//
// THREAD SAFETY. `OnDrawCompleted` and `Evaluate` run on Archicad's render
// thread inside a detour, under `ScopedInjectionGuard`. `SetEnabled`, `Reset`
// and `CopyResults` are main thread; nothing here may outlive Archicad's device.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace checkpoints {

// Run forty-nine measured at most five draws per frame into the scene depth
// view, so eight is headroom rather than a guess -- and `overflowed` says when
// that stops being true.
constexpr uint32_t kCapacity = 8;

// ⚠️ RECORDED, NOT USED TO CLASSIFY. See the header note.
struct Attributes {
    bool used = false;
    uint32_t drawOrdinal = 0;
    uint32_t drawKind = 0; // matches census::DrawKind
    uint32_t indexCount = 0;
    uint32_t topology = 0;
    uint64_t vertexShader = 0;
    uint64_t pixelShader = 0;
    uint64_t blendState = 0;
    uint64_t depthStencilState = 0;
    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    bool depthWrite = false;
    bool depthTest = false;
    uint32_t depthFunc = 0;
    bool blendEnable = false;
    uint32_t srcBlend = 0;
    uint32_t destBlend = 0;
};

struct Result {
    Attributes attributes;
    uint64_t captures = 0;
    uint64_t frontDraws = 0;
    uint64_t frontSurvived = 0;
    uint64_t behindDraws = 0;
    uint64_t behindSurvived = 0;
    uint64_t frontSamples = 0;
    uint64_t behindSamples = 0;
};

// MAIN THREAD. Off by default; this is diagnostic machinery and it is expensive.
void SetEnabled (bool enabled);
bool Enabled ();

// MAIN THREAD. Instrument one model frame in `interval`. Clamped to at least 1.
void SetInterval (uint32_t interval);

// MAIN THREAD. Clear the measurements. ⚠️ CALLED BETWEEN THE NORMAL-NAVIGATION
// AND CLOSE-ZOOM WINDOWS, because those are two different questions and one
// table that mixed them could not answer either.
void Reset ();

// RENDER THREAD, from every draw detour AFTER the call has been forwarded.
// ⚠️ AFTER, NOT BEFORE: the whole point is what the draw LEFT in the buffer.
void OnDrawCompleted (ID3D11DeviceContext* context, uint32_t drawKind, uint32_t indexCount, uint64_t modelGeneration);

// RENDER THREAD, at Present. Tests FRONT and BEHIND against every checkpoint
// captured on the frame being instrumented.
void Evaluate (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, ID3D11RenderTargetView* targetView,
               float viewportX, float viewportY, float viewportWidth, float viewportHeight);

// The checkpoint the run settled on, for the production path to bind.
ID3D11DepthStencilView* ViewAt (uint32_t index);

struct Stats {
    uint64_t framesInstrumented = 0;
    uint64_t capturesIssued = 0;
    uint64_t overflowed = 0; // more scene draws in a frame than checkpoints
    uint64_t evaluations = 0;
    uint64_t queriesIssued = 0;
    uint64_t queriesResolved = 0;
    uint32_t used = 0;
    uint32_t interval = 12;
    uint32_t width = 0, height = 0;
    bool ready = false;
    bool enabled = false;
    char lastError[160] = {};
};
Stats GetStats ();
uint32_t CopyResults (Result* out, uint32_t capacity);

// MAIN THREAD, at teardown.
void Shutdown ();

} // namespace checkpoints
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
