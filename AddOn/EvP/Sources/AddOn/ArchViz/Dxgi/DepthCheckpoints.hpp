#ifndef EVP_ARCHVIZ_DXGI_DEPTHCHECKPOINTS_HPP
#define EVP_ARCHVIZ_DXGI_DEPTHCHECKPOINTS_HPP

// WHERE, inside one Archicad frame, its depth buffer stops being usable by the
// overlay (PLAT-RE155, docs/architecture/api/HANDOFF-OverlayPatch.md stage 8).
//
// ⚠️ TWO INSTRUMENTS FAILED BEFORE THIS ONE AND BOTH FAILURES ARE THE DESIGN.
//
// Run forty-nine tried to find the opaque-to-transparent boundary from D3D
// state, and measured that ALL 2914 draws into the scene depth view have
// `BlendEnable = TRUE`. Archicad expresses opacity through the blend EQUATION,
// not the enable bit, so there was no boundary in the state to find.
//
// Run fifty copied the depth buffer after each draw and tested each copy with
// the FRONT/BEHIND probe pair. It reported `GOOD MODEL DEPTH` at every
// checkpoint while the user was looking at visibly wrong occlusion -- and it
// overflowed, sampling 8 of the frame's 28 draws. Two separate faults:
//
//   ⚠️ FULL-RESOLUTION COPIES CAPPED THE TABLE. Eight depth textures is 200 MB;
//      twenty-eight would be 700. The instrument ran out of room exactly where
//      it would have become informative.
//
//   ⚠️ AND THE PROBE PAIR IS A POINT TEST WHILE THE CONTAMINATION IS REGIONAL.
//      FRONT and BEHIND sit at a fixed offset from the anchor. A build plane can
//      leave the anchor untouched and blank the overlay everywhere else, which
//      is precisely what "GOOD at every checkpoint, wrong on screen" means.
//
// So this copies nothing and probes in place. After every draw into the scene
// depth view it runs three tests against Archicad's LIVE buffer, with depth
// writes off and colour writes off, so nothing it does can alter the frame:
//
//     FRONT   a primitive in front of the model. Should survive.
//     BEHIND  a primitive behind it. Should be rejected.
//     GHOST   ⚠️ THE WHOLE MESH, WHICH IS THE ONE THAT MATTERS. It spans a
//             region rather than a point, so a plane that occludes it somewhere
//             shows up as a drop in its sample count even when the anchor is
//             perfectly behaved.
//
// The contaminator is the draw after which GHOST falls sharply while BEHIND is
// still being rejected. The draw's attributes are recorded beside each
// checkpoint and take NO PART in choosing it -- they exist to name the boundary
// once the samples have located it.
//
// ⚠️ NOTHING HERE WRITES A PIXEL OR A DEPTH VALUE. Colour write mask zero, depth
// write mask zero. The occlusion query counts samples that would have passed;
// the frame Archicad is composing is bit-for-bit what it would have been.
//
// THREAD SAFETY. `OnDrawCompleted` and `Resolve` run on Archicad's render thread
// inside a detour, under `ScopedInjectionGuard`. `SetEnabled`, `Reset` and
// `CopyResults` are main thread; nothing here may outlive Archicad's device.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11RenderTargetView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace checkpoints {

// ⚠️ FORTY-EIGHT, BECAUSE RUN FIFTY MEASURED TWENTY-EIGHT AND OVERFLOWED AT
// EIGHT. Each checkpoint is now three occlusion queries and no texture at all,
// so the ceiling costs kilobytes rather than hundreds of megabytes -- and
// `overflowed` still says when even this stops being enough.
constexpr uint32_t kCapacity = 48;

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
    bool depthWrite = false;
    bool depthTest = false;
    uint32_t depthFunc = 0;
    bool blendEnable = false;
    uint32_t srcBlend = 0;
    uint32_t destBlend = 0;
};

struct Result {
    Attributes attributes;
    uint64_t rounds = 0; // completed measurements at this checkpoint
    uint64_t frontSurvived = 0;
    uint64_t behindSurvived = 0;
    uint64_t ghostSurvived = 0;
    uint64_t frontSamples = 0;
    uint64_t behindSamples = 0;
    uint64_t ghostSamples = 0;
};

// MAIN THREAD. Off by default; this is diagnostic machinery.
void SetEnabled (bool enabled);
bool Enabled ();

// MAIN THREAD. Instrument one model frame in `interval`. Clamped to at least 1.
void SetInterval (uint32_t interval);

// MAIN THREAD. Clear the measurements, keeping the device objects.
void Reset ();

// RENDER THREAD, from every draw detour AFTER the call has been forwarded.
// ⚠️ AFTER, NOT BEFORE: the question is what the draw LEFT in the buffer.
void OnDrawCompleted (ID3D11DeviceContext* context, uint32_t drawKind, uint32_t indexCount, uint64_t modelGeneration);

// RENDER THREAD, at Present. Collects whatever query results are ready.
void Resolve (ID3D11DeviceContext* context);

struct Stats {
    uint64_t framesInstrumented = 0;
    uint64_t roundsIssued = 0;
    uint64_t overflowed = 0; // more scene draws in a frame than checkpoints
    uint64_t queriesIssued = 0;
    uint64_t queriesResolved = 0;
    uint64_t skippedNoCamera = 0;
    uint64_t skippedNoPipeline = 0;
    uint32_t used = 0;
    uint32_t drawsLastFrame = 0;
    uint32_t interval = 10;
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
