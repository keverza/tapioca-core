#ifndef EVP_ARCHVIZ_DXGI_CONTEXTHOOKSELFTEST_HPP
#define EVP_ARCHVIZ_DXGI_CONTEXTHOOKSELFTEST_HPP

// A device of our own to prove the patched vtable on (PLAT-RE153,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 1).
//
// WHY IT IS ITS OWN FILE. `ContextHook.cpp` owns two things: WHICH vtable slots
// are patched, and WHETHER the detour in each one was reached. This file owns
// the third -- how to make a D3D11 device that is nobody's but ours, and how to
// call each of those eleven methods on it legally. That is a different subject
// with a different failure mode (a resource could not be created) and it is what
// pushed the hook over the size cap; the seam is where the checker asked for one.
//
// ⚠️ THE THROWAWAY MUST BE CREATED WITH ARCHICAD'S OWN CREATION FLAGS AND
// FEATURE LEVEL, AND THE FIRST LIVE RUN IS WHY. That run patched the vtable read
// off a throwaway made with default flags, self-tested all eleven slots green,
// and then recorded ZERO detour entries over 724 Archicad frames -- not one,
// from any context in the process, including our own Diligent renderer drawing
// continuously. The table we patched was real and reachable and simply nobody
// else's: D3D11 does not have ONE `ID3D11DeviceContext` implementation. The
// runtime wraps the immediate context (the thread-safety layer, the debug layer,
// and different concrete classes per feature level), so a device created with
// different flags dispatches through a different vtable entirely. A self-test
// that passes on our object proves nothing about Archicad's unless the two share
// an implementation.
//
// So the hook now reads the table off ARCHICAD'S OWN CONTEXT, and this file's
// job is to produce a throwaway that lands on that same table -- by asking
// Archicad's device for its `GetCreationFlags` and `GetFeatureLevel` and asking
// for exactly those. `Create` reports the vtable it got; the caller compares it
// with the target's and refuses if they differ, because a self-test it cannot
// run is not a self-test it may skip.
//
// ⚠️ THE VERDICT IS NOT HERE, DELIBERATELY. `Exercise` calls the methods and
// returns; it does not know what a detour is and cannot say whether one fired.
// The caller compares its own per-slot counters, because the counters are the
// hook's state and moving them here would split one fact across two files.
//
// ⚠️ NOTHING HERE EVER CALLS A METHOD ON ARCHICAD'S CONTEXT. An immediate
// context is single-threaded by D3D11's contract and belongs to Archicad's
// render thread; driving it from ours to see whether a hook fires would corrupt
// the state of a live frame to test a diagnostic. The throwaway exists precisely
// so the calls land somewhere harmless.

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace selftest {

struct Throwaway {
    ID3D11Device*           device = nullptr;
    ID3D11DeviceContext*    context = nullptr;
    ID3D11Buffer*           dynamicBuffer = nullptr;
    ID3D11Buffer*           defaultBuffer = nullptr;
    ID3D11Texture2D*        colour = nullptr;
    ID3D11Texture2D*        depth = nullptr;
    ID3D11RenderTargetView* colourView = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    void**                  vtable = nullptr;

    void Release ();
};

// MAIN THREAD. Create a device with `creationFlags` at `featureLevel`, build the
// 1x1 targets and 256-byte buffers `Exercise` needs, and report the vtable its
// immediate context dispatches through.
//
// ⚠️ PASS ARCHICAD'S FLAGS AND LEVEL, NOT ZERO. See the header: a mismatch here
// is the difference between hooking the table Archicad uses and hooking one
// nobody does, and the second failure is silent.
// `creationFlags` is Archicad's `ID3D11Device::GetCreationFlags`, `featureLevel`
// its `GetFeatureLevel`. Spelled as plain integers rather than `UINT` and
// `D3D_FEATURE_LEVEL` so this header stays free of <windows.h> and <d3d11.h>,
// like its neighbours in this directory.
bool Create (unsigned int creationFlags, int featureLevel, Throwaway& out,
             std::string& error);

// MAIN THREAD. Call every method in the discovery set on `throwaway.context`,
// once each, with legal arguments.
//
// ⚠️ IT RUNS WITH THE VTABLE ALREADY PATCHED, which means Archicad's own calls
// are passing through the detours for the few milliseconds this takes. They are
// filtered out by object pointer, so the cost to Archicad is a load, a compare
// and a tail call. Nominating Archicad's context BEFORE this would instead let a
// mis-indexed slot record garbage from a live frame -- which is the exact thing
// being guarded against.
void Exercise (const Throwaway& throwaway);

// One patched slot, for `ValidateTable`.
struct SlotDescriptor {
    size_t      index;
    const char* name;
};

// Structural checks on a vtable BEFORE anything is written to it: every named
// slot points into d3d11.dll, and no two of them point at the same function.
//
// ⚠️ THESE CARRY MORE WEIGHT THAN THEY USED TO. With the call-based self-test
// only available when a throwaway lands on the same table -- which on 2026-09-13
// it did not, even with Archicad's own creation flags -- the module check and an
// n-way distinctness check are what stands between a correct ABI assumption and
// writing a function pointer into an unrelated slot of a live d3d11.dll. False
// with `error` filled names the slot that failed and which check it failed.
bool ValidateTable (void** vtable, const SlotDescriptor* slots, size_t count,
                    std::string& error);

// The module a vtable slot points into, lower-cased, or empty. Used to refuse a
// table that is not d3d11.dll's before anything is written to it -- if the
// indices or the layout were ever wrong, the alternative is patching an
// arbitrary pointer in somebody else's module, which is a crash with no
// diagnosis. Same check, same reason, as the present hook's dxgi.dll one.
std::string OwningModuleOf (const void* function);

}   // namespace selftest
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
