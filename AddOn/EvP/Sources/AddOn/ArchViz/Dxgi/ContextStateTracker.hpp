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
    uint32_t firstConstant = 0;   // in 16-byte constants, as D3D11 reports it
    uint32_t numConstants = 0;    // 0 when the whole buffer is bound

    uint32_t ByteOffset () const { return firstConstant * 16u; }
    bool     IsBound () const { return buffer != 0; }
};

struct ContextState {
    uint64_t vertexShader = 0;
    ConstantBufferBinding vsConstantBuffers[kConstantBufferSlots];

    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;

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

// ---- any thread ------------------------------------------------------------
// A copy of what is bound now. This is what a scene pass snapshots when it
// begins, so the pass inherits bindings made long before it rather than waiting
// for them to be repeated.
ContextState Snapshot ();

void Reset ();

}   // namespace contextstate
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
