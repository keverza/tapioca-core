#include "ArchViz/DiligentNativeInterop.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <d3d12.h>
#include <dxgi1_4.h>

#include <CommandQueueD3D12.h>
#include <DeviceContextD3D11.h>
#include <DeviceContextD3D12.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <RenderDeviceD3D12.h>
#include <Texture.h>

#include <cstdio>

namespace geomsrv::archviz {
namespace {

using Diligent::RefCntAutoPtr;

std::string HrText (HRESULT hr)
{
    char text[32] = {};
    std::snprintf (text, sizeof (text), "0x%08lX", static_cast<unsigned long> (hr));
    return text;
}

} // namespace

RenderBackend BackendOfDevice (Diligent::IRenderDevice* device)
{
    return device != nullptr && device->GetDeviceInfo ().Type == Diligent::RENDER_DEVICE_TYPE_D3D12
               ? RenderBackend::D3D12
               : RenderBackend::D3D11;
}

RenderBackend BackendOfContext (Diligent::IDeviceContext* context)
{
    if (context == nullptr)
        return RenderBackend::D3D11;
    RefCntAutoPtr<Diligent::IDeviceContextD3D12> d3d12 { context, Diligent::IID_DeviceContextD3D12 };
    return d3d12 != nullptr ? RenderBackend::D3D12 : RenderBackend::D3D11;
}

void ReleaseNativeBindings (Diligent::IDeviceContext* context)
{
    if (context == nullptr)
        return;
    if (BackendOfContext (context) == RenderBackend::D3D12) {
        // Nothing on the CPU side holds a D3D12 back buffer the way a D3D11
        // context does; what can is work the GPU has not finished. Drain it.
        context->Flush ();
        context->FinishFrame ();
        context->WaitForIdle ();
        return;
    }
    RefCntAutoPtr<Diligent::IDeviceContextD3D11> nativeContext { context, Diligent::IID_DeviceContextD3D11 };
    if (nativeContext != nullptr) {
        if (ID3D11DeviceContext* d3dContext = nativeContext->GetD3D11DeviceContext ()) {
            // InvalidateState does NOT merely drop Diligent's cache: it nulls
            // six shader stages and the RTV on the native context, but
            // leaves shader resources and samplers BOUND. ClearState is
            // the one that releases everything. DILIGENT-BOUNDARY-PLAN.md.
            d3dContext->ClearState ();
            d3dContext->Flush ();
        }
    }
    else {
        ArchVizLog ("Diligent viewport: could not reach the native D3D11 context to "
                    "clear it -- reopening the viewport may fail with 'no swap chain'");
    }
}

void SyncBeforeStagingRead (Diligent::IDeviceContext* context)
{
    // ⚠️ WaitForIdle, NOT A FENCE PER READBACK. Both callers read rarely -- a
    // click, a capture -- and a one-frame stall on those is invisible, whereas a
    // fence the code forgot to signal is a readback that silently returns the
    // previous frame's bytes.
    if (context != nullptr && BackendOfContext (context) == RenderBackend::D3D12)
        context->WaitForIdle ();
}

void EndNativeFrame (Diligent::IDeviceContext* context)
{
    if (context != nullptr && BackendOfContext (context) == RenderBackend::D3D12)
        context->FinishFrame ();
}

bool NativeClearABAvailable (Diligent::IDeviceContext* context)
{
    return BackendOfContext (context) == RenderBackend::D3D11;
}

bool CreateCompositionChainD3D12 (Diligent::IDeviceContext* context, const void* desc, IDXGISwapChain1*& chain,
                                  std::string& error)
{
    chain = nullptr;
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = ::CreateDXGIFactory2 (0, __uuidof (IDXGIFactory2), reinterpret_cast<void**> (&factory));
    if (FAILED (hr) || factory == nullptr) {
        error = "CreateDXGIFactory2 " + HrText (hr);
        return false;
    }
    // ⚠️ THE QUEUE IS LOCKED FOR THE CALL AND ONLY FOR THE CALL. D3D12 creates a
    // swap chain against the queue that will present it; Diligent owns that
    // queue and must not be submitting to it while DXGI binds the chain.
    Diligent::ICommandQueue* queueBase = context->LockCommandQueue ();
    RefCntAutoPtr<Diligent::ICommandQueueD3D12> queue { queueBase, Diligent::IID_CommandQueueD3D12 };
    ID3D12CommandQueue* nativeQueue = queue != nullptr ? queue->GetD3D12CommandQueue () : nullptr;
    if (nativeQueue == nullptr) {
        context->UnlockCommandQueue ();
        factory->Release ();
        error = "the immediate context exposed no D3D12 command queue";
        return false;
    }
    hr = factory->CreateSwapChainForComposition (nativeQueue, static_cast<const DXGI_SWAP_CHAIN_DESC1*> (desc), nullptr,
                                                 &chain);
    context->UnlockCommandQueue ();
    factory->Release ();
    if (FAILED (hr) || chain == nullptr) {
        error = "CreateSwapChainForComposition (D3D12) " + HrText (hr);
        return false;
    }
    return true;
}

void* AcquireCompositionBufferD3D12 (IDXGISwapChain1* chain)
{
    // ⚠️ THE CURRENT INDEX, NOT BUFFER 0. D3D11 renames buffer 0 to whichever
    // buffer is next; D3D12 does not, and drawing into buffer 0 every frame
    // presents every other frame and shows the other one stale.
    IDXGISwapChain3* chain3 = nullptr;
    if (chain == nullptr ||
        FAILED (chain->QueryInterface (__uuidof (IDXGISwapChain3), reinterpret_cast<void**> (&chain3))) ||
        chain3 == nullptr)
        return nullptr;
    const UINT index = chain3->GetCurrentBackBufferIndex ();
    chain3->Release ();
    ID3D12Resource* buffer = nullptr;
    if (FAILED (chain->GetBuffer (index, __uuidof (ID3D12Resource), reinterpret_cast<void**> (&buffer))))
        return nullptr;
    return buffer;
}

void ReleaseCompositionBuffer (void* buffer)
{
    if (buffer != nullptr)
        static_cast<IUnknown*> (buffer)->Release ();
}

void WrapCompositionBufferD3D12 (Diligent::IRenderDevice* device, void* buffer, Diligent::ITexture** texture)
{
    *texture = nullptr;
    RefCntAutoPtr<Diligent::IRenderDeviceD3D12> device12 { device, Diligent::IID_RenderDeviceD3D12 };
    if (device12 == nullptr || buffer == nullptr)
        return;
    // PRESENT: where a buffer DXGI hands back is. D1 wrapped it the same way.
    device12->CreateTextureFromD3DResource (static_cast<ID3D12Resource*> (buffer), Diligent::RESOURCE_STATE_PRESENT,
                                            texture);
}

void PrepareCompositionPresentD3D12 (Diligent::IDeviceContext* context, Diligent::ITexture* drawn)
{
    RefCntAutoPtr<Diligent::IDeviceContextD3D12> context12 { context, Diligent::IID_DeviceContextD3D12 };
    if (context12 == nullptr)
        return;
    if (drawn != nullptr)
        context12->TransitionTextureState (drawn, D3D12_RESOURCE_STATE_PRESENT);
    context->Flush ();
}

void IdleBeforeCompositionResizeD3D12 (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context)
{
    if (context != nullptr) {
        context->Flush ();
        context->FinishFrame ();
    }
    // IdleGPU waits for every queue AND releases the stale resources, which is
    // where the wrappers just dropped are actually destroyed.
    if (device != nullptr)
        device->IdleGPU ();
}

} // namespace geomsrv::archviz
