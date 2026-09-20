// ArchViz/Dxgi/ContextHookDetours -- the eleven-and-counting detours
// themselves. `ContextHook.cpp` owns the lifecycle around them; see
// ContextHookShared.hpp for why the two are separate translation units.
//
// Every detour has the same shape and it is not negotiable: bump the drain
// counter, decide the audience, do the cheap recording if it is Archicad's and
// the slot is on, then tail-call the original READ INTO A LOCAL FIRST -- Remove
// can null it between the check and the call, and a null call here takes
// Archicad's render thread with it.

#include "ArchViz/Dxgi/ContextHookShared.hpp"
#include "ArchViz/Dxgi/MarkerLadder.hpp"

#include "ArchViz/Dxgi/ContextEventRing.hpp"
#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/DepthCheckpoints.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/PassProvenance.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>

#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace hookshared {

// ---- the shared state, defined here and declared in the header -------------

std::atomic<uint64_t> g_firstCallUs { 0 };
std::atomic<uint64_t> g_lastCallUs { 0 };

uint64_t MicrosecondsNow ()
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    if (!QueryPerformanceFrequency (&frequency) || frequency.QuadPart == 0 || !QueryPerformanceCounter (&counter))
        return 0;
    return uint64_t (counter.QuadPart * 1000000ll / frequency.QuadPart);
}

// ---- vtable indices --------------------------------------------------------
// IUnknown 0-2, ID3D11DeviceChild 3-6, then ID3D11DeviceContext from 7. Taken
// from `ID3D11DeviceContextVtbl` in the Windows SDK's own `d3d11.h`, which is
// the declaration order the C++ vtable is generated from -- not from memory and
// not from a blog post.
//
// ⚠️ THEY ARE STILL NOT TRUSTED. The COM ABI fixes the order, and on top of that
// every slot is checked to live in d3d11.dll, checked to be distinct from the
// other ten, and fingerprinted into the patch profile. Where a throwaway device
// lands on the same table they are additionally proven by CALLING them. A wrong
// index writes a function pointer into an unrelated slot, and the failure is a
// crash inside the driver with nothing pointing at us.
//
// ⚠️ `extern const`, NOT `constexpr`. A namespace-scope const has internal
// linkage by default, so the plain spelling would give each translation unit its
// own copy and the shared declaration would not resolve.
extern const size_t kSlotIndex[size_t (ContextSlot::Count)] = {
    44, // RSSetViewports
    45, // RSSetScissorRects
    33, // OMSetRenderTargets
    8,  // PSSetShaderResources
    7,  // VSSetConstantBuffers
    16, // PSSetConstantBuffers
    22, // GSSetConstantBuffers
    14, // Map
    15, // Unmap
    48, // UpdateSubresource
    50, // ClearRenderTargetView
    53, // ClearDepthStencilView
    12, // DrawIndexed
    13, // Draw
    20, // DrawIndexedInstanced
    47, // CopyResource
    58, // ExecuteCommandList
    21, // DrawInstanced
    38, // DrawAuto
    39, // DrawIndexedInstancedIndirect
    40, // DrawInstancedIndirect
    41, // Dispatch
    42, // DispatchIndirect
    46, // CopySubresourceRegion
    57, // ResolveSubresource
    // ⚠️ ID3D11DeviceContext1 TERRITORY, PAST THE BASE INTERFACE'S 114 SLOTS.
    // Same object and same table -- a derived COM interface extends its base's
    // layout -- so these are reachable from the pointer we already hold. They are
    // also where a D3D11.1 renderer actually binds its constant buffers.
    119, // VSSetConstantBuffers1
    123, // PSSetConstantBuffers1
    116, // UpdateSubresource1
};

// One past the highest index patched. Keeps the writable window small, for the
// reason `PresentHook::WithWritableVtable` gives.
// ⚠️ IT REACHES INTO ID3D11DeviceContext1 NOW, so the writable window grew from
// 59 pointers to 124. `ValidateTable` checks the whole range is committed and
// that every slot we touch points into d3d11.dll before anything is written --
// which matters more here than it did at 59, because Archicad's table is
// inline in the context object rather than in .rdata, and a table shorter than
// we assumed would put a write into the object's own fields.
extern const size_t kVtableSlots = 124;

using RSSetViewportsFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRectsFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
using PSSetShaderResourcesFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT,
                                                          ID3D11ShaderResourceView* const*);
using SetConstantBuffersFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                            D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                       const void*, UINT, UINT);
using ClearRTVFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDSVFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using DrawIndexedFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using CopyResourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ExecuteCommandListFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using DrawInstancedFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*);
using DrawIndirectFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispatchFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, UINT);
using DispatchIndirectFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using CopySubresourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                                     ID3D11Resource*, UINT, const D3D11_BOX*);
using ResolveSubresourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*,
                                                        UINT, DXGI_FORMAT);
using SetConstantBuffers1Fn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*,
                                                         const UINT*, const UINT*);
using UpdateSubresource1Fn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                        const void*, UINT, UINT, UINT);

// The drain counter -- see PresentHook for the argument. Remove restores the
// pointers and then spins until this reads zero.
std::atomic<int32_t> g_inFlight { 0 };

// ⚠️ THE FILTER. Every detour compares against this and returns immediately when
// it does not match; without it the hook records our own Diligent renderer as
// though it were Archicad. `g_selfTestContext` is the same idea for the
// throwaway used at install time, kept separate so a self-test can never leave
// data in the real rings.
std::atomic<uint64_t> g_archicadContext { 0 };
std::atomic<uint64_t> g_selfTestContext { 0 };

std::atomic<bool> g_slotEnabled[size_t (ContextSlot::Count)];
std::atomic<uint64_t> g_perSlot[size_t (ContextSlot::Count)];
std::atomic<uint64_t> g_selfTestSeen[size_t (ContextSlot::Count)];

std::atomic<uint64_t> g_calls { 0 };

// ⚠️ ON ITS OWN CACHE LINE, AND THAT IS NOT A MICRO-OPTIMISATION. This is the one
// counter written by a thread that is NOT Archicad's -- our own Diligent renderer
// drives its context through the same patched vtable -- so sharing a line with
// `g_calls` would have two render threads invalidating each other's cache
// thousands of times a frame. The cost would land as Archicad frame time, which
// is exactly what PLAT-RE118 measured this path to be sensitive to and exactly
// the thing this hook must not do.
alignas (64) std::atomic<uint64_t> g_otherContextCalls { 0 };
char g_otherContextCallsPadding[64 - sizeof (std::atomic<uint64_t>)] = {};

bool SlotOn (ContextSlot slot)
{
    return g_slotEnabled[size_t (slot)].load (std::memory_order_relaxed);
}

Audience Who (ID3D11DeviceContext* context, ContextSlot slot)
{
    const uint64_t key = uint64_t (uintptr_t (context));
    if (key == 0)
        return Audience::Ignore;

    // ⚠️ OUR OWN INJECTED CALLS ARE NOT ARCHICAD'S, AND NO POINTER FILTER CAN
    // TELL THEM APART. The injection renderer drives ARCHICAD'S context on
    // purpose, so its VSSetShader, VSSetConstantBuffers1 and Draw* arrive here on
    // the same object with the same key. Without this the injected triangle
    // would overwrite the tracked camera bindings with its own, count itself as
    // a scene draw, advance the scene-pass state and feed its constants to the
    // classifier -- the instrument measuring itself. The guard is checked here,
    // once, because every recording path in this file goes through `Who`.
    if (contextstate::Injecting ())
        return Audience::Ignore;

    if (key == g_archicadContext.load (std::memory_order_acquire)) {
        g_calls.fetch_add (1, std::memory_order_relaxed);
        g_perSlot[size_t (slot)].fetch_add (1, std::memory_order_relaxed);
        // ⚠️ TWO RELAXED STORES ON THE HOT PATH, and they are worth it. Without
        // the span, "four draws" cannot be told from "four draws and then the
        // hook stopped", and those two readings send the next week of work in
        // opposite directions. `compare_exchange` on the first is only contended
        // once, on the very first call of a run.
        const uint64_t now = MicrosecondsNow ();
        uint64_t expected = 0;
        g_firstCallUs.compare_exchange_strong (expected, now, std::memory_order_relaxed);
        g_lastCallUs.store (now, std::memory_order_relaxed);
        return Audience::Archicad;
    }
    if (key == g_selfTestContext.load (std::memory_order_acquire)) {
        g_selfTestSeen[size_t (slot)].fetch_add (1, std::memory_order_relaxed);
        return Audience::SelfTest;
    }
    // ⚠️ THE FILTERED-OUT COUNT IS ONLY TAKEN WHEN THE SLOT IS ON, and that is
    // the difference between this hook costing Archicad nothing and costing it
    // a contended atomic per call. Every call from OUR renderer's context lands
    // here, thousands per frame, on a thread that is not Archicad's; counting
    // them unconditionally would make a DISABLED slot as expensive as an enabled
    // one, which defeats the per-slot gating the whole design rests on. What is
    // lost is the count from a disabled slot, and that number describes traffic
    // nobody asked to see.
    if (SlotOn (slot))
        g_otherContextCalls.fetch_add (1, std::memory_order_relaxed);
    return Audience::Ignore;
}

class PassProvenanceOperation {
  public:
    explicit PassProvenanceOperation (bool archicad) : active (archicad && passprovenance::BeginContextOperation ())
    {
    }

    ~PassProvenanceOperation ()
    {
        Finish ();
    }

    void Finish ()
    {
        if (!active)
            return;
        passprovenance::EndContextOperation (active);
        active = false;
    }

  private:
    bool active;
};

// ---- the detours -----------------------------------------------------------
// Every one of them: bump the drain counter, decide the audience, do the cheap
// recording if it is Archicad's and the slot is on, then tail-call the original
// read into a LOCAL first -- Remove can null it between the check and the call,
// and a null call here takes Archicad's render thread with it.

void STDMETHODCALLTYPE DetourRSSetViewports (ID3D11DeviceContext* context, UINT count, const D3D11_VIEWPORT* viewports)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::RSSetViewports) == Audience::Archicad && SlotOn (ContextSlot::RSSetViewports)) {
        if (count > 0 && viewports != nullptr) {
            renderstate::OnViewport (viewports[0]);
            contextstate::OnViewport (viewports[0]);
            eventring::Record (ContextSlot::RSSetViewports, 0, count, uint32_t (viewports[0].Width),
                               uint32_t (viewports[0].Height));
        }
        else {
            eventring::Record (ContextSlot::RSSetViewports, 0, count, 0, 0);
        }
    }
    const RSSetViewportsFn original = OriginalOf<RSSetViewportsFn> (ContextSlot::RSSetViewports);
    if (original != nullptr)
        original (context, count, viewports);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourRSSetScissorRects (ID3D11DeviceContext* context, UINT count, const D3D11_RECT* rects)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::RSSetScissorRects) == Audience::Archicad &&
        SlotOn (ContextSlot::RSSetScissorRects)) {
        if (count > 0 && rects != nullptr) {
            renderstate::OnScissor (rects[0]);
            eventring::Record (ContextSlot::RSSetScissorRects, 0, count, uint32_t (rects[0].right - rects[0].left),
                               uint32_t (rects[0].bottom - rects[0].top));
        }
        else {
            eventring::Record (ContextSlot::RSSetScissorRects, 0, count, 0, 0);
        }
    }
    const RSSetScissorRectsFn original = OriginalOf<RSSetScissorRectsFn> (ContextSlot::RSSetScissorRects);
    if (original != nullptr)
        original (context, count, rects);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourOMSetRenderTargets (ID3D11DeviceContext* context, UINT count,
                                                 ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::OMSetRenderTargets) == Audience::Archicad &&
        SlotOn (ContextSlot::OMSetRenderTargets)) {
        ID3D11RenderTargetView* first = (count > 0 && targets != nullptr) ? targets[0] : nullptr;
        // ⚠️ THE SCENE-COMPLETION TRIGGER FIRES HERE, BEFORE THE SWITCH IS
        // FORWARDED, so Archicad's scene target, depth buffer and viewport are
        // still the bound state its own draws used.
        //
        // ⚠️ IT IS THIS AND NOT `CopyResource` BECAUSE ARCHICAD NEVER COPIES THE
        // SCENE. Run twenty-one: zero `ResolveSubresource` (the target is not
        // MSAA) and zero of thirty-six `CopyResource` calls sourced the scene
        // colour -- the 3D pass renders straight into the swap-chain back buffer
        // and is consumed by Present itself. There is no consumer operation to
        // hang a causal trigger on, so the transition away from the target is
        // what is left. Runs nineteen through twenty-one recorded `targetReturns
        // = 0`, meaning Archicad does not come back to it, so the first
        // departure IS the final one -- and the pass counts returns so that
        // stops being an assumption the moment it is false.
        if (renderstate::SceneCompletesAt (uint64_t (uintptr_t (first)))) {
            // ⚠️ THE `AfterTransparent` MOMENT IS HERE, AND IT IS TAKEN
            // WHETHER OR NOT ANYTHING INJECTS. The scene-pass injection point is
            // off when the overlay draws at Present, but the depth snapshot the
            // sweep is comparing has to be taken all the same.
            injection::depth::OnScenePassEnd (context, renderstate::ModelSceneGeneration ());
            injection::InjectIfReady (context);

            // ⚠️ RUNGS B AND C USED TO BE HERE AND HAVE BEEN
            // REMOVED. They called `OMGetRenderTargets`, `QueryInterface` and
            // `ClearView` on Archicad's own context FROM INSIDE A DETOUR ON THAT
            // CONTEXT, BEFORE the original `OMSetRenderTargets` was forwarded --
            // so the driver was asked to perform a clear while it was part way
            // through a render-target transition. They also held no
            // `ScopedInjectionGuard`, which every other call we make on
            // Archicad's context does hold, so our own clears were being recorded
            // by our own detours as Archicad's activity.
            //
            // ⚠️ AND THEY HAD ALREADY REPORTED. B was visible
            // in all three modes and C in none, which is the whole answer they
            // existed to give: Archicad's scene colour target reaches the screen
            // and the target it switches to does not. Carrying a re-entrancy
            // hazard in the hottest detour in the tree, for a question that is
            // answered, is not a trade worth making -- Archicad crashed on a
            // Floor Plan -> 3D transition on 2026-09-19 at 17:34 with the ladder
            // armed, immediately after this hook installed.
            //
            // Rungs A and E remain: A runs inside the census, which already holds
            // the guard, and E runs in the Present detour, which is not a context
            // detour at all.
        }

        contextstate::OnRenderTargets (first, depth);
        renderstate::OnRenderTargets (first, depth);
        passprovenance::OnRenderTargets (count, targets);
        // `handle` is the colour target; the depth target rides in b/c as the low
        // and high halves of its pointer, because the ring row is fixed-size and
        // the two must stay in the SAME row -- a depth bind logged separately
        // could be reordered against its colour bind by the ring and the pair is
        // the whole content of the event.
        const uint64_t dsv = uint64_t (uintptr_t (depth));
        eventring::Record (ContextSlot::OMSetRenderTargets, uint64_t (uintptr_t (first)), count,
                           uint32_t (dsv & 0xffffffffull), uint32_t (dsv >> 32));
    }
    const OMSetRenderTargetsFn original = OriginalOf<OMSetRenderTargetsFn> (ContextSlot::OMSetRenderTargets);
    if (original != nullptr)
        original (context, count, targets, depth);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourPSSetShaderResources (ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                                   ID3D11ShaderResourceView* const* views)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::PSSetShaderResources) == Audience::Archicad &&
        SlotOn (ContextSlot::PSSetShaderResources))
        passprovenance::OnPSShaderResources (startSlot, count, views);
    const PSSetShaderResourcesFn original = OriginalOf<PSSetShaderResourcesFn> (ContextSlot::PSSetShaderResources);
    if (original != nullptr)
        original (context, startSlot, count, views);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void RecordConstantBuffers (ContextSlot slot, UINT startSlot, UINT count, ID3D11Buffer* const* buffers)
{
    // ⚠️ THE STATE TRACKER IS FED FROM THE VERTEX-STAGE BINDS WHATEVER ELSE
    // HAPPENS, because a binding that is never repeated is exactly the one an
    // event recorder misses. See ContextStateTracker.hpp.
    if (slot == ContextSlot::VSSetConstantBuffers)
        contextstate::OnVSConstantBuffers (startSlot, count, buffers, nullptr, nullptr);
    ID3D11Buffer* first = (count > 0 && buffers != nullptr) ? buffers[0] : nullptr;
    viewmatrix::OnConstantBufferBound (uint32_t (slot), startSlot, first);
    eventring::Record (slot, uint64_t (uintptr_t (first)), startSlot, count, 0);
}

void STDMETHODCALLTYPE DetourVSSetConstantBuffers (ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                                   ID3D11Buffer* const* buffers)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::VSSetConstantBuffers) == Audience::Archicad &&
        SlotOn (ContextSlot::VSSetConstantBuffers))
        RecordConstantBuffers (ContextSlot::VSSetConstantBuffers, startSlot, count, buffers);
    const SetConstantBuffersFn original = OriginalOf<SetConstantBuffersFn> (ContextSlot::VSSetConstantBuffers);
    if (original != nullptr)
        original (context, startSlot, count, buffers);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourPSSetConstantBuffers (ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                                   ID3D11Buffer* const* buffers)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::PSSetConstantBuffers) == Audience::Archicad &&
        SlotOn (ContextSlot::PSSetConstantBuffers))
        RecordConstantBuffers (ContextSlot::PSSetConstantBuffers, startSlot, count, buffers);
    const SetConstantBuffersFn original = OriginalOf<SetConstantBuffersFn> (ContextSlot::PSSetConstantBuffers);
    if (original != nullptr)
        original (context, startSlot, count, buffers);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourGSSetConstantBuffers (ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                                   ID3D11Buffer* const* buffers)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::GSSetConstantBuffers) == Audience::Archicad &&
        SlotOn (ContextSlot::GSSetConstantBuffers))
        RecordConstantBuffers (ContextSlot::GSSetConstantBuffers, startSlot, count, buffers);
    const SetConstantBuffersFn original = OriginalOf<SetConstantBuffersFn> (ContextSlot::GSSetConstantBuffers);
    if (original != nullptr)
        original (context, startSlot, count, buffers);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

HRESULT STDMETHODCALLTYPE DetourMap (ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource,
                                     D3D11_MAP mapType, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const Audience audience = Who (context, ContextSlot::Map);
    const bool archicad = audience == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);

    // ⚠️ THE ORIGINAL RUNS FIRST HERE, unlike every other detour in this file.
    // Map's whole output is the pointer it writes into `mapped`, and there is
    // nothing to remember until it has produced one.
    const MapFn original = OriginalOf<MapFn> (ContextSlot::Map);
    const HRESULT hr =
        (original != nullptr) ? original (context, resource, subresource, mapType, flags, mapped) : E_FAIL;
    if (archicad && mapType != D3D11_MAP_READ && SUCCEEDED (hr))
        passprovenance::OnResourceWrite (resource);

    if (audience == Audience::Archicad && SlotOn (ContextSlot::Map) && SUCCEEDED (hr) && mapped != nullptr &&
        mapped->pData != nullptr && subresource == 0) {
        // ⚠️ THE SIZING AND THE REMEMBERING BOTH BELONG TO THE CLASSIFIER, not
        // to the hook. What counts as a capturable buffer, and what is done with
        // the pointer between Map and Unmap, are its rules; this file only says
        // when the two calls happened.
        const uint32_t width = viewmatrix::OnMapped (resource, mapped->pData);
        eventring::Record (ContextSlot::Map, uint64_t (uintptr_t (resource)), subresource, uint32_t (mapType), width);
    }
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
    return hr;
}

void STDMETHODCALLTYPE DetourUnmap (ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::Unmap) == Audience::Archicad && SlotOn (ContextSlot::Unmap)) {
        // ⚠️ BEFORE THE ORIGINAL. After Unmap the pointer is no longer ours to
        // read -- the runtime may have handed the allocation back or recycled it
        // -- so a copy taken afterwards is a read of freed memory that will
        // usually appear to work.
        const uint32_t bytes = viewmatrix::OnUnmapping (resource);
        eventring::Record (ContextSlot::Unmap, uint64_t (uintptr_t (resource)), subresource, bytes, 0);
    }
    const UnmapFn original = OriginalOf<UnmapFn> (ContextSlot::Unmap);
    if (original != nullptr)
        original (context, resource, subresource);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourUpdateSubresource (ID3D11DeviceContext* context, ID3D11Resource* resource,
                                                UINT subresource, const D3D11_BOX* box, const void* source,
                                                UINT rowPitch, UINT depthPitch)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::UpdateSubresource) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad && SlotOn (ContextSlot::UpdateSubresource) && source != nullptr && subresource == 0) {
        const uint32_t width = viewmatrix::ConstantBufferWidth (resource);
        // ⚠️ A BOX MEANS A PARTIAL UPDATE, and its left/right are BYTES for a
        // buffer. Copying `width` bytes from `source` in that case would read
        // past the caller's data -- the source only holds the sub-range.
        uint32_t offset = 0;
        uint32_t bytes = width;
        if (box != nullptr) {
            offset = box->left;
            bytes = (box->right > box->left) ? (box->right - box->left) : 0;
        }
        if (width > 0 && bytes > 0 && offset + bytes <= width)
            viewmatrix::OnConstantBufferWrite (resource, offset, source, bytes, 0);
        eventring::Record (ContextSlot::UpdateSubresource, uint64_t (uintptr_t (resource)), subresource, bytes,
                           rowPitch);
    }
    const UpdateSubresourceFn original = OriginalOf<UpdateSubresourceFn> (ContextSlot::UpdateSubresource);
    if (original != nullptr)
        original (context, resource, subresource, box, source, rowPitch, depthPitch);
    if (archicad)
        passprovenance::OnResourceWrite (resource);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourClearRenderTargetView (ID3D11DeviceContext* context, ID3D11RenderTargetView* view,
                                                    const FLOAT colour[4])
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::ClearRenderTargetView) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad && SlotOn (ContextSlot::ClearRenderTargetView)) {
        renderstate::OnClearRenderTarget (view);
        eventring::Record (ContextSlot::ClearRenderTargetView, uint64_t (uintptr_t (view)), 0, 0, 0);
    }
    const ClearRTVFn original = OriginalOf<ClearRTVFn> (ContextSlot::ClearRenderTargetView);
    if (original != nullptr)
        original (context, view, colour);
    if (archicad)
        passprovenance::OnClearRenderTarget (view);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourClearDepthStencilView (ID3D11DeviceContext* context, ID3D11DepthStencilView* view,
                                                    UINT flags, FLOAT depth, UINT8 stencil)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::ClearDepthStencilView) == Audience::Archicad &&
        SlotOn (ContextSlot::ClearDepthStencilView)) {
        renderstate::OnClearDepthStencil (view);
        uint32_t depthBits = 0;
        std::memcpy (&depthBits, &depth, sizeof (depthBits));
        eventring::Record (ContextSlot::ClearDepthStencilView, uint64_t (uintptr_t (view)), flags, depthBits, stencil);
    }
    const ClearDSVFn original = OriginalOf<ClearDSVFn> (ContextSlot::ClearDepthStencilView);
    if (original != nullptr)
        original (context, view, flags, depth, stencil);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

// ⚠️ THE DRAW DETOURS RECORD NOTHING INTO THE RING, ONLY A COUNT. They are the
// hottest functions in the API by an order of magnitude -- a scene pass issues
// thousands per frame -- and what is being asked of them is one number: does
// Archicad draw on this context at all. A ring entry per draw would cost
// Archicad real frame time to answer a yes/no question.
// ⚠️ AFTER THE FORWARD, BECAUSE THE QUESTION IS WHAT THE DRAW LEFT
// BEHIND. Every draw detour calls this once the real call has executed, so a
// depth checkpoint captures the buffer as that draw finished it, not as it found
// it. It is a no-op unless the checkpoint diagnostic is armed.
void PostDraw (ID3D11DeviceContext* context, uint32_t kind, UINT count, bool archicad)
{
    if (archicad)
        passprovenance::OnDrawCompleted ();
    if (injection::checkpoints::Enabled ())
        injection::checkpoints::OnDrawCompleted (context, kind, uint32_t (count), renderstate::ModelSceneGeneration ());
}

void STDMETHODCALLTYPE DetourDrawIndexed (ID3D11DeviceContext* context, UINT indexCount, UINT startIndex,
                                          INT baseVertex)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::DrawIndexed) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        if (renderstate::OnDraw ())
            injection::SnapshotCamera (context);
        // ⚠️ THE CENSUS SEES EVERY DRAW, NOT ONLY THE LEARNED PASS'S. It is off
        // unless a diagnostic arms it, it draws nothing, and it admits a draw on
        // its own evidence -- both camera windows bound at the 256-byte shape --
        // rather than on the learner's verdict. See CameraCensus.hpp.
        census::OnDraw (context, census::DrawKind::Indexed, indexCount);
    }
    const DrawIndexedFn original = OriginalOf<DrawIndexedFn> (ContextSlot::DrawIndexed);
    if (original != nullptr)
        original (context, indexCount, startIndex, baseVertex);
    PostDraw (context, 0, indexCount, archicad);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourDraw (ID3D11DeviceContext* context, UINT count, UINT start)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::Draw) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        if (renderstate::OnDraw ())
            injection::SnapshotCamera (context);
        // ⚠️ THE CENSUS SEES EVERY DRAW, NOT ONLY THE LEARNED PASS'S. It is off
        // unless a diagnostic arms it, it draws nothing, and it admits a draw on
        // its own evidence -- both camera windows bound at the 256-byte shape --
        // rather than on the learner's verdict. See CameraCensus.hpp.
        census::OnDraw (context, census::DrawKind::Direct, count);
        renderstate::NoteDirectDraw (count);
    }
    const DrawFn original = OriginalOf<DrawFn> (ContextSlot::Draw);
    if (original != nullptr)
        original (context, count, start);
    PostDraw (context, 1, count, archicad);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourDrawIndexedInstanced (ID3D11DeviceContext* context, UINT perInst, UINT instances,
                                                   UINT startIndex, INT baseVertex, UINT startInstance)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::DrawIndexedInstanced) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        if (renderstate::OnDraw ())
            injection::SnapshotCamera (context);
        // ⚠️ THE CENSUS SEES EVERY DRAW, NOT ONLY THE LEARNED PASS'S. It is off
        // unless a diagnostic arms it, it draws nothing, and it admits a draw on
        // its own evidence -- both camera windows bound at the 256-byte shape --
        // rather than on the learner's verdict. See CameraCensus.hpp.
        census::OnDraw (context, census::DrawKind::IndexedInstanced, perInst);
    }
    const DrawIndexedInstFn original = OriginalOf<DrawIndexedInstFn> (ContextSlot::DrawIndexedInstanced);
    if (original != nullptr)
        original (context, perInst, instances, startIndex, baseVertex, startInstance);
    PostDraw (context, 2, perInst, archicad);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

extern // ⚠️ THESE TWO ARE THE FOURTH RUN'S QUESTION, and it is a sharp one. That run
       // found Archicad presenting 2981 frames through this swap chain while its
       // immediate context issued FOUR draw calls -- and the device is not D3D11On12,
       // so the D3D12 explanation is gone. Something is putting pixels in that back
       // buffer without drawing them here, and there are only two ordinary ways:
       //
       //   * `ExecuteCommandList` -- Archicad records the scene on DEFERRED contexts,
       //     on worker threads, and the immediate context only plays the lists back.
       //     That is standard multithreaded D3D11, it would explain every number, and
       //     it is good news: deferred contexts have their own (per-object) vtables and
       //     the camera constant buffer is written on them, so the hook moves rather
       //     than dies.
       //   * `CopyResource` -- the frame is produced elsewhere entirely and blitted in,
       //     in which case the next question is who produced the source surface.
       //
       // A count each is enough to tell them apart, so like the draws these record
       // nothing into the ring.
    void STDMETHODCALLTYPE DetourCopyResource (ID3D11DeviceContext* context, ID3D11Resource* destination,
                                               ID3D11Resource* source)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::CopyResource) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        // ⚠️ THE SCENE-COMPLETION TRIGGER FIRES HERE, BEFORE THE COPY IS
        // FORWARDED. At this instant Archicad's scene is finished and the copy
        // that consumes it has not happened, so anything drawn now is inside the
        // frame about to be posted and presented. See RenderStateCapture.hpp.
        renderstate::OnCopyOrResolve (uint64_t (uintptr_t (source)));
    }
    const CopyResourceFn original = OriginalOf<CopyResourceFn> (ContextSlot::CopyResource);
    if (original != nullptr)
        original (context, destination, source);
    if (archicad)
        passprovenance::OnCopyResource (destination, source);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourExecuteCommandList (ID3D11DeviceContext* context, ID3D11CommandList* list,
                                                 BOOL restoreState)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::ExecuteCommandList) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    const ExecuteCommandListFn original = OriginalOf<ExecuteCommandListFn> (ContextSlot::ExecuteCommandList);
    if (original != nullptr)
        original (context, list, restoreState);
    if (archicad)
        passprovenance::OnUnsupportedGpuWork ();
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

// ---- the gap the sixth run exposed -----------------------------------------
// The draw family, the two ways to fill a target without drawing, and the
// D3D11.1 constant-buffer setters. The draws and copies are pure counters for
// the same reason the first three are; the constant-buffer ones are NOT -- they
// are what stage 3 has been waiting for.

void STDMETHODCALLTYPE DetourDrawInstanced (ID3D11DeviceContext* context, UINT perInstance, UINT instances,
                                            UINT startVertex, UINT startInstance)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::DrawInstanced) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        if (renderstate::OnDraw ())
            injection::SnapshotCamera (context);
        // ⚠️ THE CENSUS SEES EVERY DRAW, NOT ONLY THE LEARNED PASS'S. It is off
        // unless a diagnostic arms it, it draws nothing, and it admits a draw on
        // its own evidence -- both camera windows bound at the 256-byte shape --
        // rather than on the learner's verdict. See CameraCensus.hpp.
        census::OnDraw (context, census::DrawKind::Instanced, perInstance);
    }
    const DrawInstancedFn original = OriginalOf<DrawInstancedFn> (ContextSlot::DrawInstanced);
    if (original != nullptr)
        original (context, perInstance, instances, startVertex, startInstance);
    PostDraw (context, 3, perInstance, archicad);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourDrawAuto (ID3D11DeviceContext* context)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::DrawAuto) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        if (renderstate::OnDraw ())
            injection::SnapshotCamera (context);
        // ⚠️ THE CENSUS SEES EVERY DRAW, NOT ONLY THE LEARNED PASS'S. It is off
        // unless a diagnostic arms it, it draws nothing, and it admits a draw on
        // its own evidence -- both camera windows bound at the 256-byte shape --
        // rather than on the learner's verdict. See CameraCensus.hpp.
        census::OnDraw (context, census::DrawKind::Auto, 0);
    }
    const DrawAutoFn original = OriginalOf<DrawAutoFn> (ContextSlot::DrawAuto);
    if (original != nullptr)
        original (context);
    if (archicad)
        passprovenance::OnDrawCompleted ();
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect (ID3D11DeviceContext* context, ID3D11Buffer* args,
                                                           UINT offset)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::DrawIndexedInstancedIndirect) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        if (renderstate::OnDraw ())
            injection::SnapshotCamera (context);
        // ⚠️ THE CENSUS SEES EVERY DRAW, NOT ONLY THE LEARNED PASS'S. It is off
        // unless a diagnostic arms it, it draws nothing, and it admits a draw on
        // its own evidence -- both camera windows bound at the 256-byte shape --
        // rather than on the learner's verdict. See CameraCensus.hpp.
        census::OnDraw (context, census::DrawKind::IndexedInstancedIndirect, 0);
    }
    const DrawIndirectFn original = OriginalOf<DrawIndirectFn> (ContextSlot::DrawIndexedInstancedIndirect);
    if (original != nullptr)
        original (context, args, offset);
    if (archicad)
        passprovenance::OnDrawCompleted ();
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourDrawInstancedIndirect (ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::DrawInstancedIndirect) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        if (renderstate::OnDraw ())
            injection::SnapshotCamera (context);
        // ⚠️ THE CENSUS SEES EVERY DRAW, NOT ONLY THE LEARNED PASS'S. It is off
        // unless a diagnostic arms it, it draws nothing, and it admits a draw on
        // its own evidence -- both camera windows bound at the 256-byte shape --
        // rather than on the learner's verdict. See CameraCensus.hpp.
        census::OnDraw (context, census::DrawKind::InstancedIndirect, 0);
    }
    const DrawIndirectFn original = OriginalOf<DrawIndirectFn> (ContextSlot::DrawInstancedIndirect);
    if (original != nullptr)
        original (context, args, offset);
    if (archicad)
        passprovenance::OnDrawCompleted ();
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourDispatch (ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::Dispatch) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    const DispatchFn original = OriginalOf<DispatchFn> (ContextSlot::Dispatch);
    if (original != nullptr)
        original (context, x, y, z);
    if (archicad)
        passprovenance::OnUnsupportedGpuWork ();
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourDispatchIndirect (ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::DispatchIndirect) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    const DispatchIndirectFn original = OriginalOf<DispatchIndirectFn> (ContextSlot::DispatchIndirect);
    if (original != nullptr)
        original (context, args, offset);
    if (archicad)
        passprovenance::OnUnsupportedGpuWork ();
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourCopySubresourceRegion (ID3D11DeviceContext* context, ID3D11Resource* destination,
                                                    UINT subresource, UINT x, UINT y, UINT z, ID3D11Resource* source,
                                                    UINT sourceSub, const D3D11_BOX* box)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::CopySubresourceRegion) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        renderstate::OnCopyOrResolve (uint64_t (uintptr_t (source)));
    }
    const CopySubresourceFn original = OriginalOf<CopySubresourceFn> (ContextSlot::CopySubresourceRegion);
    if (original != nullptr)
        original (context, destination, subresource, x, y, z, source, sourceSub, box);
    if (archicad)
        passprovenance::OnPartialResourceCopy (destination, source);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourResolveSubresource (ID3D11DeviceContext* context, ID3D11Resource* destination,
                                                 UINT destSub, ID3D11Resource* source, UINT sourceSub,
                                                 DXGI_FORMAT format)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::ResolveSubresource) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad) {
        renderstate::OnCopyOrResolve (uint64_t (uintptr_t (source)));
    }
    const ResolveSubresourceFn original = OriginalOf<ResolveSubresourceFn> (ContextSlot::ResolveSubresource);
    if (original != nullptr)
        original (context, destination, destSub, source, sourceSub, format);
    if (archicad)
        passprovenance::OnPartialResourceCopy (destination, source);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

// ⚠️ THESE TWO ARE NOT COUNTERS. `VSSetConstantBuffers1` is where a D3D11.1
// renderer binds the buffer the camera lives in, so they feed the classifier
// exactly as the base-interface versions do. The extra `firstConstant` /
// `numConstants` arrays are the whole point of the `1` variants -- a bind can
// now name a RANGE inside a larger buffer -- and stage 3 will need that offset
// when it comes to score, because the matrix may not start at byte zero.
void RecordConstantBuffers1 (ContextSlot slot, UINT startSlot, UINT count, ID3D11Buffer* const* buffers,
                             const UINT* firstConstant, const UINT* numConstants)
{
    // ⚠️ `numConstants` IS PART OF THE BINDING AND MUST NOT BE DROPPED. Restoring
    // a window with the right buffer and the right offset but the wrong LENGTH
    // exposes a different region of an 8 MiB ring, and the injected shader would
    // read whatever happened to follow the camera. It was being passed as
    // nullptr, which is why every reported `numConstants` read 0.
    if (slot == ContextSlot::VSSetConstantBuffers1)
        contextstate::OnVSConstantBuffers (startSlot, count, buffers, firstConstant, numConstants);
    if (buffers == nullptr || count == 0) {
        eventring::Record (slot, 0, startSlot, count, 0);
        return;
    }

    // ⚠️ EVERY BUFFER IN THE CALL, NOT JUST `buffers[0]`. This read only the
    // first for three runs, and a bind of several constant buffers in one call
    // is ordinary -- b0 the per-frame constants, b1 the per-object, b2 the
    // material. If Archicad's camera rides in anything but the first register of
    // a multi-buffer bind, the window carrying it was never even offered to the
    // classifier, and stage 3 was scoring the other registers and reporting NO
    // MATCH. Twenty-odd thousand binds a run: whichever one holds the camera,
    // it has to be in the set.
    //
    // `firstConstant` is in 16-byte constants, not bytes. The event ring records
    // it raw so the reader converts once; the classifier is told in bytes,
    // because that is what it will index a mapped pointer with.
    for (UINT i = 0; i < count; ++i) {
        ID3D11Buffer* const buffer = buffers[i];
        if (buffer == nullptr)
            continue;
        const uint32_t offset = (firstConstant != nullptr) ? firstConstant[i] : 0;
        // ⚠️ THIS IS THE CALL THAT MAKES STAGE 3 POSSIBLE ON THIS HOST. Archicad
        // 29 keeps its constants in one 8 MiB ring and binds windows of it, so
        // the buffer pointer alone says nothing -- the offset is the address.
        viewmatrix::OnConstantBufferBoundWindow (uint32_t (slot), startSlot + i, buffer, offset * 16u);
    }

    // One ring row for the call, naming its first buffer: the ring is a trace of
    // WHEN things happened and it already drops most of a busy frame, so a row
    // per buffer would cost coverage of everything else to say what the
    // classifier is being told anyway.
    eventring::Record (slot, uint64_t (uintptr_t (buffers[0])), startSlot, count,
                       (firstConstant != nullptr) ? firstConstant[0] : 0);
}

void STDMETHODCALLTYPE DetourVSSetConstantBuffers1 (ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                                    ID3D11Buffer* const* buffers, const UINT* firstConstant,
                                                    const UINT* numConstants)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::VSSetConstantBuffers1) == Audience::Archicad &&
        SlotOn (ContextSlot::VSSetConstantBuffers1))
        RecordConstantBuffers1 (ContextSlot::VSSetConstantBuffers1, startSlot, count, buffers, firstConstant,
                                numConstants);
    const SetConstantBuffers1Fn original = OriginalOf<SetConstantBuffers1Fn> (ContextSlot::VSSetConstantBuffers1);
    if (original != nullptr)
        original (context, startSlot, count, buffers, firstConstant, numConstants);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourPSSetConstantBuffers1 (ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                                    ID3D11Buffer* const* buffers, const UINT* firstConstant,
                                                    const UINT* numConstants)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    if (Who (context, ContextSlot::PSSetConstantBuffers1) == Audience::Archicad &&
        SlotOn (ContextSlot::PSSetConstantBuffers1))
        RecordConstantBuffers1 (ContextSlot::PSSetConstantBuffers1, startSlot, count, buffers, firstConstant,
                                numConstants);
    const SetConstantBuffers1Fn original = OriginalOf<SetConstantBuffers1Fn> (ContextSlot::PSSetConstantBuffers1);
    if (original != nullptr)
        original (context, startSlot, count, buffers, firstConstant, numConstants);
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void STDMETHODCALLTYPE DetourUpdateSubresource1 (ID3D11DeviceContext* context, ID3D11Resource* resource,
                                                 UINT subresource, const D3D11_BOX* box, const void* source,
                                                 UINT rowPitch, UINT depthPitch, UINT copyFlags)
{
    g_inFlight.fetch_add (1, std::memory_order_acquire);
    const bool archicad = Who (context, ContextSlot::UpdateSubresource1) == Audience::Archicad;
    PassProvenanceOperation provenanceOperation (archicad);
    if (archicad && SlotOn (ContextSlot::UpdateSubresource1) && source != nullptr && subresource == 0) {
        const uint32_t width = viewmatrix::ConstantBufferWidth (resource);
        uint32_t offset = 0;
        uint32_t bytes = width;
        if (box != nullptr) {
            offset = box->left;
            bytes = (box->right > box->left) ? (box->right - box->left) : 0;
        }
        if (width > 0 && bytes > 0 && offset + bytes <= width)
            viewmatrix::OnConstantBufferWrite (resource, offset, source, bytes, 0);
        eventring::Record (ContextSlot::UpdateSubresource1, uint64_t (uintptr_t (resource)), subresource, bytes,
                           rowPitch);
    }
    const UpdateSubresource1Fn original = OriginalOf<UpdateSubresource1Fn> (ContextSlot::UpdateSubresource1);
    if (original != nullptr)
        original (context, resource, subresource, box, source, rowPitch, depthPitch, copyFlags);
    if (archicad)
        passprovenance::OnResourceWrite (resource);
    provenanceOperation.Finish ();
    g_inFlight.fetch_sub (1, std::memory_order_release);
}

void* const kDetour[size_t (ContextSlot::Count)] = {
    (void*) &DetourRSSetViewports,
    (void*) &DetourRSSetScissorRects,
    (void*) &DetourOMSetRenderTargets,
    (void*) &DetourPSSetShaderResources,
    (void*) &DetourVSSetConstantBuffers,
    (void*) &DetourPSSetConstantBuffers,
    (void*) &DetourGSSetConstantBuffers,
    (void*) &DetourMap,
    (void*) &DetourUnmap,
    (void*) &DetourUpdateSubresource,
    (void*) &DetourClearRenderTargetView,
    (void*) &DetourClearDepthStencilView,
    (void*) &DetourDrawIndexed,
    (void*) &DetourDraw,
    (void*) &DetourDrawIndexedInstanced,
    (void*) &DetourCopyResource,
    (void*) &DetourExecuteCommandList,
    (void*) &DetourDrawInstanced,
    (void*) &DetourDrawAuto,
    (void*) &DetourDrawIndexedInstancedIndirect,
    (void*) &DetourDrawInstancedIndirect,
    (void*) &DetourDispatch,
    (void*) &DetourDispatchIndirect,
    (void*) &DetourCopySubresourceRegion,
    (void*) &DetourResolveSubresource,
    (void*) &DetourVSSetConstantBuffers1,
    (void*) &DetourPSSetConstantBuffers1,
    (void*) &DetourUpdateSubresource1,
};

} // namespace hookshared
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
