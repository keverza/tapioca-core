#ifndef EVP_ARCHVIZ_DXGI_CONTEXTSTATETRACKER_HPP
#define EVP_ARCHVIZ_DXGI_CONTEXTSTATETRACKER_HPP

// What is bound to Archicad's immediate context RIGHT NOW (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 4).
//
// ⚠️ WHY THIS EXISTS, AND IT IS THE CORRECTION OF A REAL MISTAKE. Everything
// before it recorded EVENTS -- a bind happened, a buffer was unmapped -- and then
// asked which events fell inside a scene pass. A D3D11 context is not a stream of
// events, it is a STATE MACHINE: a constant buffer bound once stays bound through
// every draw that follows, across depth clears and across Presents, until
// something binds over it. So a camera constant that Archicad sets up early and
// never touches again is invisible to an event recorder that starts listening at
// the depth clear.
//
// That is almost certainly why runs fifteen to eighteen found the PROJECTION
// every time -- Archicad rebinds `b2` constantly -- and the VIEW only once. The
// view was not missing. We were not looking at what was bound; we were looking
// at what had just been re-bound.
//
// ⚠️ THE AUTHORITATIVE ASSOCIATION IS NOT "this matrix was captured during pass
// N". It is "this constant-buffer window WAS BOUND when scene draw X of pass N
// executed". This file is what makes that sentence answerable.
//
// THREAD SAFETY. Every `On*` runs inside a context detour on Archicad's render
// thread, which D3D11's own contract makes single-threaded for an immediate
// context. The state is plain, not atomic, for the reason `RenderStateCapture`
// gives at length: one writer, naturally-aligned scalars, and an interlocked
// write on a path that runs thousands of times a frame would buy a guarantee this
// data does not need. `Snapshot` is the one cross-thread read and it takes a
// copy; a torn copy costs a wrong row in a report, never a crash.

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;
struct D3D11_VIEWPORT;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace contextstate {

// D3D11 gives every stage fourteen constant-buffer slots.
constexpr size_t kConstantBufferSlots = 14;

// ⚠️ `firstConstant` AND `numConstants` ARE PART OF THE BINDING, not decoration.
// Archicad binds windows of one 8 MiB ring, so the buffer pointer alone names
// megabytes; the window is the address. A slot bound through the non-windowed
// `VSSetConstantBuffers` has firstConstant 0 and numConstants 0, which means "the
// whole buffer" and is recorded as such rather than faked.
struct ConstantBufferBinding {
    uint64_t buffer = 0;
    uint32_t firstConstant = 0; // in 16-byte constants, as D3D11 reports it
    uint32_t numConstants = 0;  // 0 when the whole buffer is bound

    uint32_t ByteOffset () const
    {
        return firstConstant * 16u;
    }
    bool IsBound () const
    {
        return buffer != 0;
    }
};

// ⚠️ WHAT THE VIEW IS A VIEW OF, BECAUSE THE VIEW POINTER DOES NOT
// SURVIVE. Run forty-five is the whole argument: the census selected a camera
// group at RTV 1970460137464 / DSV 1970460135224, and by the time the injection
// was armed the SAME 3D window, the SAME shaders and the SAME viewport were
// drawing through RTV 1970460145208 / DSV 1970460147384. Archicad had recreated
// its views; nothing about the camera had changed. A recognizer keyed on a COM
// address therefore recognised nothing, and zero draws matched.
//
// The description -- width, height, format, sample count -- is a property of the
// RESOURCE and survives the view being rebuilt around it, so it is what a
// logical fingerprint can be made of.
//
// ⚠️ RESOLVED WHEN THE BOUND POINTER CHANGES, NEVER PER DRAW. This
// costs a `GetResource` + `QueryInterface` + `GetDesc` on `OMSetRenderTargets`,
// which happens tens of times a frame, rather than thousands. The one hazard is
// an address reused by a different resource with the same value, which would
// keep a stale description; the fingerprint tolerates that because it is only
// ever used to RE-ACQUIRE a concrete binding, never to keep one.
struct ViewDescriptor {
    bool present = false;
    uint32_t width = 0, height = 0;
    uint32_t format = 0;
    uint32_t sampleCount = 0;

    bool SameResource (const ViewDescriptor& other) const
    {
        return present == other.present && width == other.width && height == other.height && format == other.format &&
               sampleCount == other.sampleCount;
    }
};

struct ContextState {
    uint64_t vertexShader = 0;
    ConstantBufferBinding vsConstantBuffers[kConstantBufferSlots];

    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    // ⚠️ THE VIEW ITSELF, BECAUSE AN ID CANNOT BE AddRef'd AND
    // PRODUCTION HAS TO RETAIN ONE. `hostocclusion` builds its private depth
    // buffer by copying this view's width, height, format and sample count, so
    // the compose path needs the pointer and not just the identity the
    // fingerprint compares. Raw and unowned: it is read inside the draw detour
    // that captured it, while Archicad still has it bound, and whoever keeps it
    // past that takes a reference of their own.
    ID3D11DepthStencilView* depthStencilView = nullptr;
    ViewDescriptor renderTargetDesc;
    ViewDescriptor depthStencilDesc;

    float viewportX = 0.0f;
    float viewportY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
};

// ---- render thread, from the context detours -------------------------------
void OnVertexShader (ID3D11VertexShader* shader);
void OnVSConstantBuffers (uint32_t startSlot, uint32_t count, ID3D11Buffer* const* buffers,
                          const uint32_t* firstConstant, const uint32_t* numConstants);
void OnRenderTargets (ID3D11RenderTargetView* colour, ID3D11DepthStencilView* depth);
void OnViewport (const D3D11_VIEWPORT& viewport);

// ---- the injection reentrancy guard ----------------------------------------
// ⚠️ MANDATORY BEFORE ANYTHING INJECTS A DRAW, and object-pointer filtering
// cannot replace it. The injected renderer deliberately uses ARCHICAD'S OWN
// context -- that is the whole point -- so every `VSSetShader`,
// `VSSetConstantBuffers1`, `OMSetRenderTargets` and `Draw*` it makes arrives at
// the same detours, on the same object, indistinguishable from Archicad's by any
// filter that looks at the pointer.
//
// Without this guard the first injected triangle would: overwrite the tracked
// view and projection bindings with our own, count itself as a scene draw,
// advance the scene-pass state, and feed its own constants to the classifier.
// The instrument would be measuring itself, which is the failure this rung has
// already spent a day on for a different reason.
//
// RENDER THREAD ONLY, and it must nest correctly: the injected draw runs INSIDE
// a detour that is already forwarding one of Archicad's calls.
class ScopedInjectionGuard {
  public:
    ScopedInjectionGuard ();
    ~ScopedInjectionGuard ();
    ScopedInjectionGuard (const ScopedInjectionGuard&) = delete;
    ScopedInjectionGuard& operator= (const ScopedInjectionGuard&) = delete;
};

// True while an injected draw is in flight. Every detour checks it and, when it
// is set, forwards the call and records NOTHING.
bool Injecting ();

// ---- what the last verified Archicad scene draw consumed -------------------
// ⚠️ A PASS-START SNAPSHOT IS NOT GOOD ENOUGH, AND THIS IS THE CORRECTION.
// Between the final scene `DrawIndexed` and the `OMSetRenderTargets` that leaves
// the scene target, Archicad may legally change its shader and its constant
// buffers. A detour that reads the live state at the target switch would
// therefore inject with state that did NOT render the model. The contract has to
// be:
//
//     last Archicad scene draw
//         -> capture exactly what THAT draw consumed
//         -> candidate scene-completion boundary
//         -> inject with THAT camera state
//
// So the bindings are latched at every draw that executes while the verified
// scene RTV and DSV are bound, and the last one latched is what injection uses.
struct SceneDrawState {
    bool valid = false;
    // ⚠️ WHICH PRESENTED FRAME THIS BELONGS TO, AND IT IS NOT OPTIONAL. Run
    // twenty-five injected 1051 times from only 858 camera-bearing draws, so
    // roughly a fifth of the triangles were drawn with a camera latched in an
    // EARLIER frame. A stale but geometrically valid camera is precisely the bug
    // this whole rung exists to eliminate, and it is invisible in every counter
    // that does not compare generations.
    // The MODEL-SCENE generation this draw belonged to. See
    // `RenderStateCapture::ModelSceneGeneration` for why this is not a Present
    // counter: a camera outlives any number of Presents and is superseded only
    // by a new model scene.
    uint64_t modelSceneGeneration = 0;
    uint64_t scenePassGeneration = 0;
    uint64_t sceneTargetEpoch = 0; // increments on every (re-)entry to the target
    uint64_t drawSequence = 0;     // which draw of that epoch this was
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    float viewportX = 0.0f;
    float viewportY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    ConstantBufferBinding vsConstantBuffers[kConstantBufferSlots];
};

// RENDER THREAD. Latch the live state as the newest verified scene draw.
//
// ⚠️ RETURNS TRUE EXACTLY WHEN THIS DRAW BECAME THE NEW `LastCameraDraw` -- that
// is, when THIS draw consumed both `b1` and `b2` as 256-byte windows AND belonged
// to the learned model pass. It is the ONLY definition of "camera-bearing draw"
// in the tree, and it is returned rather than restated because a second copy of
// this predicate is exactly what went wrong: `RenderStateCapture::OnDraw` tested
// only `IsBound()`, without the window size, and so qualified 724 draws where
// this function counted 362. Every snapshot count downstream was therefore
// exactly twice the truth, and the invariant that was supposed to catch it was
// comparing two different populations.
bool OnSceneDraw (uint64_t scenePassGeneration, uint64_t sceneTargetEpoch, uint64_t drawSequence,
                  uint64_t modelSceneGeneration, bool inModelPass);

// ANY THREAD. What the most recent verified scene draw consumed.
SceneDrawState LastSceneDraw ();

// ⚠️ THE LAST DRAW THAT ACTUALLY CARRIED A CAMERA, WHICH IS NOT THE LAST DRAW.
// Archicad finishes a pass with selection markers, gizmos and screen-space
// helpers that use a different shader and different constants, so "the last
// draw of the pass" can easily be one of those -- and injecting with whatever it
// had bound would be a camera from nowhere. A draw counts here only if THAT
// EXACT DRAW consumed both `b1` and `b2` as the 256-byte windows stage 3
// identified; having had them bound earlier in the pass is not enough.
SceneDrawState LastCameraDraw ();

// How strictly that is holding. `withBoth` is the only one that matters.
struct DrawCameraCounts {
    uint64_t total = 0;
    uint64_t withView = 0;
    uint64_t withProjection = 0;
    uint64_t withBoth = 0;
    // ⚠️ AND OF THOSE, HOW MANY WERE IN THE LEARNED MODEL PASS. Run twenty-five:
    // `b2` was bound on all 1716 draws but `b1` on only 858, so slot 1 does not
    // always hold the model's view -- shadow, helper and UI passes bind their own
    // thing there. Latching any draw that merely has both slots filled hands the
    // injector a camera that belongs to a different rendering, which projects the
    // triangle somewhere off screen. It has to be the MODEL pass's camera.
    uint64_t withBothInModelPass = 0;
};
DrawCameraCounts GetDrawCameraCounts ();

// ---- any thread ------------------------------------------------------------
// A copy of what is bound now. This is what a scene pass snapshots when it
// begins, so the pass inherits bindings made long before it rather than waiting
// for them to be repeated.
ContextState Snapshot ();

void Reset ();

} // namespace contextstate
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
