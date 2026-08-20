#include "ArchViz/DiligentClearAB.hpp"

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <DeviceContextD3D11.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <RenderDeviceD3D11.h>
#include <SwapChain.h>
#include <SwapChainD3D11.h>
#include <TextureViewD3D11.h>

#include <string>

namespace geomsrv {
namespace archviz {

namespace {

// Borrowed native interfaces behind a Diligent context + RTV pair. Nothing here
// is owned: the returned pointers live exactly as long as the Diligent objects.
struct NativeTarget {
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* texture = nullptr;   // AddRef'd, release with Release()
    uint32_t textureFormat = 0;
    uint32_t viewFormat = 0;
    UINT width = 0;
    UINT height = 0;
    std::string failure;

    bool IsValid () const { return failure.empty (); }
};

NativeTarget AcquireNativeTarget (Diligent::IDeviceContext* context, Diligent::ITextureView* rtv)
{
    NativeTarget target;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContextD3D11> nativeContext {
        context, Diligent::IID_DeviceContextD3D11};
    Diligent::RefCntAutoPtr<Diligent::ITextureViewD3D11> nativeView {
        rtv, Diligent::IID_TextureViewD3D11};
    if (nativeContext == nullptr || nativeView == nullptr) {
        target.failure = "native D3D11 interfaces unavailable";
        return target;
    }
    target.context = nativeContext->GetD3D11DeviceContext ();

    ID3D11View* view = nativeView->GetD3D11View ();
    if (view == nullptr) {
        target.failure = "back-buffer view has no ID3D11View";
        return target;
    }
    // Borrowed: QueryInterface would AddRef a pointer we do not own the release
    // of, and the RTV outlives every use here.
    ID3D11RenderTargetView* renderTargetView = nullptr;
    if (SUCCEEDED (view->QueryInterface (__uuidof (ID3D11RenderTargetView),
                                         reinterpret_cast<void**> (&renderTargetView))) &&
        renderTargetView != nullptr) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
        renderTargetView->GetDesc (&rtvDesc);
        target.viewFormat = (uint32_t) rtvDesc.Format;
        target.rtv = renderTargetView;
        renderTargetView->Release ();   // the Diligent view keeps it alive
    }

    ID3D11Resource* resource = nullptr;
    view->GetResource (&resource);
    if (resource == nullptr) {
        target.failure = "back-buffer view returned no resource";
        return target;
    }
    const HRESULT queryHr = resource->QueryInterface (__uuidof (ID3D11Texture2D),
                                                      reinterpret_cast<void**> (&target.texture));
    resource->Release ();
    if (FAILED (queryHr) || target.texture == nullptr) {
        target.failure = "back-buffer resource is not ID3D11Texture2D";
        return target;
    }

    D3D11_TEXTURE2D_DESC desc {};
    target.texture->GetDesc (&desc);
    target.textureFormat = (uint32_t) desc.Format;
    target.width = desc.Width;
    target.height = desc.Height;
    if (target.rtv == nullptr)
        target.viewFormat = target.textureFormat;
    return target;
}

// Copies the centre pixel out of the back buffer. The caller must have unbound
// the render target first: D3D11 refuses to copy a resource that is still bound
// for output, and the first PLAT-RE22 diagnostic reported a zero pixel for
// exactly that reason rather than for a rendering fault.
ClearReadback ReadBackCenterPixel (const NativeTarget& target)
{
    ClearReadback readback;
    readback.textureFormat = target.textureFormat;
    readback.viewFormat = target.viewFormat;
    if (!target.IsValid ()) {
        readback.failure = target.failure;
        return readback;
    }

    ID3D11Device* device = nullptr;
    target.texture->GetDevice (&device);
    if (device == nullptr) {
        readback.failure = "back-buffer texture has no device";
        return readback;
    }

    D3D11_TEXTURE2D_DESC stagingDesc {};
    stagingDesc.Width = 1;
    stagingDesc.Height = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = (DXGI_FORMAT) target.textureFormat;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    const HRESULT createHr = device->CreateTexture2D (&stagingDesc, nullptr, &staging);
    device->Release ();
    if (FAILED (createHr) || staging == nullptr) {
        readback.failure = "failed to create 1x1 staging texture, HRESULT=" +
                           std::to_string ((uint32_t) createHr);
        return readback;
    }

    const UINT x = target.width / 2;
    const UINT y = target.height / 2;
    const D3D11_BOX box {x, y, 0, x + 1, y + 1, 1};
    target.context->CopySubresourceRegion (staging, 0, 0, 0, 0, target.texture, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    const HRESULT mapHr = target.context->Map (staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED (mapHr)) {
        const auto* bytes = static_cast<const uint8_t*> (mapped.pData);
        for (int i = 0; i < 4; ++i)
            readback.bytes[i] = bytes[i];
        readback.mapped = true;
        target.context->Unmap (staging, 0);
    } else {
        readback.failure = "failed to map the cleared back buffer, HRESULT=" +
                           std::to_string ((uint32_t) mapHr);
    }
    staging->Release ();
    return readback;
}

}   // namespace

DeviceDiagnostics DescribeDevice (Diligent::IRenderDevice* device, Diligent::ISwapChain* swapChain)
{
    DeviceDiagnostics diag;
    if (device != nullptr) {
        diag.adapter = device->GetAdapterInfo ().Description;
        diag.report = "adapter=\"" + diag.adapter + "\"";

        Diligent::RefCntAutoPtr<Diligent::IRenderDeviceD3D11> nativeDevice {
            device, Diligent::IID_RenderDeviceD3D11};
        if (nativeDevice != nullptr) {
            ID3D11Device* d3dDevice = nativeDevice->GetD3D11Device ();
            if (d3dDevice != nullptr) {
                diag.featureLevel = (uint32_t) d3dDevice->GetFeatureLevel ();
                diag.deviceRemovedReason = (uint32_t) d3dDevice->GetDeviceRemovedReason ();
                diag.report += " featureLevel=" + std::to_string (diag.featureLevel) +
                               " deviceRemovedReason=" + std::to_string (diag.deviceRemovedReason);
            }
        }
    }
    if (swapChain != nullptr) {
        Diligent::RefCntAutoPtr<Diligent::ISwapChainD3D11> nativeSwapChain {
            swapChain, Diligent::IID_SwapChainD3D11};
        if (nativeSwapChain != nullptr && nativeSwapChain->GetDXGISwapChain () != nullptr) {
            // Not the same number as the frame counter: that one counts calls,
            // this one counts presentations DXGI actually accepted.
            UINT presents = 0;
            if (SUCCEEDED (nativeSwapChain->GetDXGISwapChain ()->GetLastPresentCount (&presents))) {
                diag.presentCount = presents;
                diag.report += " lastPresentCount=" + std::to_string (presents);
            }
        }
    }
    return diag;
}

// The PLAT-RE22 discriminator. Clears the SAME back-buffer view twice in one
// frame -- once through Diligent, once through raw D3D11 -- and reads the
// centre pixel back after each. Which arms come back zero says where the fault
// is, and no other single observation does:
//
//   both match      -> presentation is fine; a black VIEWPORT is a window or
//                      compositor problem, not a rendering one
//   only native     -> Diligent's state/dispatch on this device
//   neither         -> the device or the swap-chain path underneath both
//
// The caller must have unbound the render target before calling: see
// ReadBackCenterPixel.
ClearAB RunClearAB (Diligent::IDeviceContext* context, Diligent::ITextureView* rtv,
                    Diligent::ISwapChain* swapChain, Diligent::IRenderDevice* device)
{
    ClearAB result;
    result.device = DescribeDevice (device, swapChain);

    const NativeTarget target = AcquireNativeTarget (context, rtv);

    // Arm A reads back the clear the caller already issued through Diligent.
    result.diligentArm = EvaluateClear (ReadBackCenterPixel (target), kDiligentClearColor,
                                        kClearTolerance);

    // Arm B goes around Diligent entirely, on the same context and the same
    // view. Nothing else about the frame changes.
    ClearReadback nativeReadback;
    nativeReadback.textureFormat = target.textureFormat;
    nativeReadback.viewFormat = target.viewFormat;
    if (!target.IsValid ()) {
        nativeReadback.failure = target.failure;
    } else if (target.rtv == nullptr) {
        nativeReadback.failure = "back-buffer view is not an ID3D11RenderTargetView";
    } else {
        target.context->ClearRenderTargetView (target.rtv, kNativeClearColor);
        // Diligent caches bindings; a native call it did not see through must be
        // announced or the next Diligent call may restore stale state over it.
        context->InvalidateState ();
        nativeReadback = ReadBackCenterPixel (target);
    }
    result.nativeArm = EvaluateClear (nativeReadback, kNativeClearColor, kClearTolerance);

    if (result.diligentArm.matched && result.nativeArm.matched)
        result.verdict = "PASS -- both arms wrote the requested colour; rendering and readback are sound";
    else if (result.nativeArm.matched)
        result.verdict = "Diligent arm only failed -- the fault is in Diligent state/dispatch, not the device";
    else if (result.diligentArm.matched)
        result.verdict = "native arm only failed -- suspect the readback path, not Diligent";
    else
        result.verdict = "BOTH arms failed -- the fault is below Diligent (device, swap chain, or driver)";

    if (target.texture != nullptr)
        target.texture->Release ();
    return result;
}

}   // namespace archviz
}   // namespace geomsrv
