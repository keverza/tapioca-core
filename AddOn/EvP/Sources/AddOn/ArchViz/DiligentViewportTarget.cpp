#include "ArchViz/DiligentViewportTarget.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <dcomp.h>
#include <dxgi1_3.h>   // IDXGISwapChain2::SetMaximumFrameLatency

#include <DeviceContextD3D11.h>
#include <EngineFactoryD3D11.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <RenderDeviceD3D11.h>
#include <SwapChain.h>
#include <SwapChainD3D11.h>
#include <Texture.h>
#include <TextureView.h>

#include <atomic>
#include <string>
#include <unordered_map>

namespace geomsrv {
namespace archviz {

namespace {

using Diligent::RefCntAutoPtr;

// The palette child's clear: a neutral light grey, the ordinary background for a
// model viewer and the one that does not tint the user's judgement of a surface
// colour sitting on it.
//
// ⚠️ IT WAS AN UNMISTAKABLE CYAN, AND THAT WAS NOT A STYLE CHOICE -- it was the
// PLAT-RE22 smoke test's instrument, chosen because the dark blue-green before it
// "could be described as black" and a black viewport is the signature of a device
// that never rendered. That question is long settled (the viewport has drawn real
// geometry since 2026-08-10), so the diagnostic value is spent and the cost --
// a saturated cast over every material in the scene -- is not worth paying.
//
// ⚠️ THE VALUES ARE LINEAR, because the render target is _SRGB and the hardware
// encodes on write. 0.62 linear lands near 0.82 sRGB on screen: clearly light,
// still dark enough that a white wall reads as white against it.
constexpr float kOpaqueClear[4] = {0.62f, 0.62f, 0.63f, 1.0f};
// The overlay's. See the header: premultiplied transparency is exactly one
// value, and any other alpha-zero colour is an additive haze.
constexpr float kTransparentClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};

std::string HrText (HRESULT hr)
{
    char buf[32] = {};
    std::snprintf (buf, sizeof (buf), "0x%08lX", (unsigned long) hr);
    return std::string (buf);
}

}   // namespace

struct DiligentViewportTarget::Impl {
    SurfaceMode mode = SurfaceMode::PaletteChild;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pendingWidth = 0;
    uint32_t pendingHeight = 0;
    bool resizePending = false;
    bool resizeFailed = false;

    // ---- present accounting (PLAT-RE99) ------------------------------------
    // Written from the render thread, read from the main thread for the state
    // command, so atomic. Latency 0 means "never set -- DXGI's default of 3".
    std::atomic<uint32_t> frameLatency {0};
    std::atomic<uint32_t> lastPresentResult {0};
    std::atomic<uint64_t> presentFailures {0};

    Diligent::IRenderDevice* device = nullptr;   // borrowed; the caller owns it
    RefCntAutoPtr<Diligent::IRenderDeviceD3D11> deviceD3D11;

    // ---- the palette-child path -------------------------------------------
    RefCntAutoPtr<Diligent::ISwapChain> swapChain;

    // ---- the overlay path --------------------------------------------------
    // ⚠️ RAW COM POINTERS WITH EXPLICIT Release, NOT ComPtr. `wrl/client.h` is
    // fine in the standalone probe, but the .apx builds under `/Zc:wchar_t-` and
    // this file already has to be careful about what it drags into a TU that
    // also sees the DevKit. Five pointers with one teardown path is cheaper than
    // finding out which ATL/WRL header disagrees with the flag.
    IDXGISwapChain1* compositionSwapChain = nullptr;
    IDCompositionDevice* dcompDevice = nullptr;
    IDCompositionTarget* dcompTarget = nullptr;
    IDCompositionVisual* dcompVisual = nullptr;
    RefCntAutoPtr<Diligent::ITexture> depthTexture;

    // ⚠️ ONE Diligent ITexture PER NATIVE BACK BUFFER, CACHED BY POINTER. The
    // flip chain rotates its buffers, so GetBuffer(0) returns a different
    // ID3D11Texture2D after every Present. Wrapping afresh every frame works and
    // allocates a Diligent object per frame; with BufferCount=2 this map holds
    // two entries and then never grows, which is also a cheap check that the
    // rotation is what we think it is. Probe 1b established the pattern.
    struct Wrapped {
        RefCntAutoPtr<Diligent::ITexture> texture;
        Diligent::ITextureView* view = nullptr;   // owned by `texture`
    };
    std::unordered_map<ID3D11Texture2D*, Wrapped> wrapped;

    uint32_t colorFormat = 0;
    uint32_t depthFormat = 0;

    bool CreateOverlayDepth (std::string& error);
    void ReleaseOverlay ();
};

bool DiligentViewportTarget::Impl::CreateOverlayDepth (std::string& error)
{
    depthTexture.Release ();
    Diligent::TextureDesc dd;
    dd.Name = "ArchViz overlay depth";
    dd.Type = Diligent::RESOURCE_DIM_TEX_2D;
    dd.Width = width;
    dd.Height = height;
    dd.MipLevels = 1;
    dd.Format = static_cast<Diligent::TEXTURE_FORMAT> (depthFormat);
    dd.BindFlags = Diligent::BIND_DEPTH_STENCIL;
    dd.Usage = Diligent::USAGE_DEFAULT;
    device->CreateTexture (dd, nullptr, &depthTexture);
    if (depthTexture == nullptr) {
        // ⚠️ THE OVERLAY CANNOT RUN WITHOUT ITS OWN DEPTH BUFFER, and that is the
        // one structural difference from the palette path. A composition swap
        // chain has NO depth buffer -- `ISwapChain::GetDepthBufferDSV` has no
        // analogue here -- so if this fails there is nothing to fall back on and
        // the scene would draw with a depth-enabled pipeline and no DSV bound,
        // which is a validation error rather than a picture.
        error = "could not create the overlay's depth buffer (" + std::to_string (width) + "x" +
                std::to_string (height) + ")";
        return false;
    }
    return true;
}

void DiligentViewportTarget::Impl::ReleaseOverlay ()
{
    // ⚠️ THE VISUAL TREE COMES DOWN BEFORE ITS CONTENT DOES, AND THE COMMIT IS
    // NOT OPTIONAL. Without SetRoot(nullptr) + Commit, DWM can keep compositing
    // a visual whose swap chain we have already released -- for a frame or two
    // in the best case. WaitForCommitCompletion is what makes "already" true.
    if (dcompDevice != nullptr && dcompTarget != nullptr) {
        dcompTarget->SetRoot (nullptr);
        dcompDevice->Commit ();
        dcompDevice->WaitForCommitCompletion ();
    }
    wrapped.clear ();
    depthTexture.Release ();
    if (dcompVisual != nullptr) { dcompVisual->Release (); dcompVisual = nullptr; }
    if (dcompTarget != nullptr) { dcompTarget->Release (); dcompTarget = nullptr; }
    if (dcompDevice != nullptr) { dcompDevice->Release (); dcompDevice = nullptr; }
    if (compositionSwapChain != nullptr) {
        compositionSwapChain->Release ();
        compositionSwapChain = nullptr;
    }
}

DiligentViewportTarget::DiligentViewportTarget () : impl_ (std::make_unique<Impl> ()) {}
DiligentViewportTarget::~DiligentViewportTarget () { Destroy (nullptr); }

bool DiligentViewportTarget::Create (Diligent::IRenderDevice* device,
                                     Diligent::IDeviceContext* context,
                                     Diligent::IEngineFactoryD3D11* factory, SurfaceMode mode,
                                     void* hwnd, uint32_t width, uint32_t height,
                                     std::string& error)
{
    if (device == nullptr || context == nullptr || factory == nullptr || hwnd == nullptr ||
        width == 0 || height == 0) {
        error = "DiligentViewportTarget::Create needs a device, a context, a factory, a window "
                "and a non-zero size";
        return false;
    }

    impl_->device = device;
    impl_->mode = mode;
    impl_->width = width;
    impl_->height = height;

    if (mode == SurfaceMode::PaletteChild) {
        Diligent::SwapChainDesc desc;
        desc.Width = width;
        desc.Height = height;
        const Diligent::Win32NativeWindow window {static_cast<HWND> (hwnd)};
        factory->CreateSwapChainD3D11 (device, context, desc, Diligent::FullScreenModeDesc {},
                                       window, &impl_->swapChain);
        if (impl_->swapChain == nullptr) {
            error = "CreateSwapChainD3D11 returned no swap chain";
            return false;
        }
        const Diligent::SwapChainDesc& actual = impl_->swapChain->GetDesc ();
        impl_->colorFormat = uint32_t (actual.ColorBufferFormat);
        impl_->depthFormat = uint32_t (actual.DepthBufferFormat);
        return true;
    }

    // ---- the overlay: DirectComposition ------------------------------------
    impl_->deviceD3D11 = RefCntAutoPtr<Diligent::IRenderDeviceD3D11> (
        device, Diligent::IID_RenderDeviceD3D11);
    if (impl_->deviceD3D11 == nullptr) {
        error = "the Diligent device does not expose IRenderDeviceD3D11, so the composition back "
                "buffer cannot be wrapped";
        return false;
    }
    ID3D11Device* nativeDevice = impl_->deviceD3D11->GetD3D11Device ();
    if (nativeDevice == nullptr) {
        error = "IRenderDeviceD3D11::GetD3D11Device returned nothing";
        return false;
    }

    // The DXGI factory that made this device. ⚠️ ASKED OF THE DEVICE, NOT
    // CreateDXGIFactory. A factory created from scratch belongs to no adapter in
    // particular, and `CreateSwapChainForComposition` on a device from a
    // different factory is one of the ways that call returns E_INVALIDARG with
    // nothing further to say.
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory2 = nullptr;
    HRESULT hr = nativeDevice->QueryInterface (__uuidof (IDXGIDevice),
                                               reinterpret_cast<void**> (&dxgiDevice));
    if (SUCCEEDED (hr))
        hr = dxgiDevice->GetAdapter (&adapter);
    if (SUCCEEDED (hr))
        hr = adapter->GetParent (__uuidof (IDXGIFactory2), reinterpret_cast<void**> (&factory2));
    if (adapter != nullptr)
        adapter->Release ();
    if (FAILED (hr) || factory2 == nullptr) {
        if (dxgiDevice != nullptr)
            dxgiDevice->Release ();
        if (factory2 != nullptr)
            factory2->Release ();
        error = "could not reach IDXGIFactory2 from the Diligent device (" + HrText (hr) + ")";
        return false;
    }

    // ⚠️ EVERY ONE OF THESE FIELDS IS CONSTRAINED, AND GETTING ANY OF THEM WRONG
    // RETURNS E_INVALIDARG WITH NO FURTHER DETAIL. A composition swap chain must
    // use a FLIP effect, DXGI_SCALING_STRETCH and BufferCount >= 2, and
    // PREMULTIPLIED is the only alpha mode it honours -- straight alpha is not an
    // option, which is why the shaders' blend states produce premultiplied
    // output. archive/experiments/TransparencyProbe/PathDComp.cpp asserts the same set by
    // construction and its header comment is the long version.
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.Stereo = FALSE;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    // ⚠️ THE WAITABLE FLAG IS WHAT MAKES FRAME LATENCY SETTABLE AT ALL.
    // IDXGISwapChain2::SetMaximumFrameLatency requires it; without it the swap
    // chain keeps DXGI's default queue depth of 3, which is up to three frames
    // of ALREADY-RENDERED overlay waiting to be shown. That is the shape of the
    // afterimage the 2026-08-13 runs kept reporting -- a stale composition
    // frame, not a failure to clear -- and it is invisible to the camera-desync
    // measurement, which timestamps submission rather than display.
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    hr = factory2->CreateSwapChainForComposition (nativeDevice, &scd, nullptr,
                                                  &impl_->compositionSwapChain);
    factory2->Release ();
    if (FAILED (hr) || impl_->compositionSwapChain == nullptr) {
        if (dxgiDevice != nullptr)
            dxgiDevice->Release ();
        error = "CreateSwapChainForComposition " + HrText (hr);
        return false;
    }

    hr = DCompositionCreateDevice (dxgiDevice, __uuidof (IDCompositionDevice),
                                   reinterpret_cast<void**> (&impl_->dcompDevice));
    if (dxgiDevice != nullptr)
        dxgiDevice->Release ();
    if (FAILED (hr) || impl_->dcompDevice == nullptr) {
        impl_->ReleaseOverlay ();
        error = "DCompositionCreateDevice " + HrText (hr);
        return false;
    }

    // topmost=TRUE: the visual tree composites above the window's own content.
    // There is none here, but the flag also decides where we sit relative to
    // child windows, and "above" is the overlay's whole premise.
    hr = impl_->dcompDevice->CreateTargetForHwnd (static_cast<HWND> (hwnd), TRUE,
                                                  &impl_->dcompTarget);
    if (SUCCEEDED (hr))
        hr = impl_->dcompDevice->CreateVisual (&impl_->dcompVisual);
    if (SUCCEEDED (hr))
        hr = impl_->dcompVisual->SetContent (impl_->compositionSwapChain);
    if (SUCCEEDED (hr))
        hr = impl_->dcompTarget->SetRoot (impl_->dcompVisual);
    if (SUCCEEDED (hr))
        hr = impl_->dcompDevice->Commit ();
    if (FAILED (hr)) {
        impl_->ReleaseOverlay ();
        error = "building the DirectComposition visual tree failed " + HrText (hr);
        return false;
    }

    impl_->colorFormat = uint32_t (Diligent::TEX_FORMAT_BGRA8_UNORM);
    impl_->depthFormat = uint32_t (Diligent::TEX_FORMAT_D32_FLOAT);
    if (!impl_->CreateOverlayDepth (error)) {
        impl_->ReleaseOverlay ();
        return false;
    }

    ArchVizLog ("ArchViz overlay target: composition swap chain " + std::to_string (width) + "x" +
                std::to_string (height) + ", BGRA8_UNORM premultiplied, FLIP_SEQUENTIAL, 2 buffers");
    return true;
}

void DiligentViewportTarget::Destroy (Diligent::IDeviceContext* context)
{
    if (impl_ == nullptr)
        return;

    // ⚠️ THE CONTEXT LETS GO FIRST, IN BOTH MODES. The flip model defers a swap
    // chain's destruction while the immediate context still references its back
    // buffers, and PLAT-RE39 is the standing proof of what that costs: the HWND
    // stays taken and every reopen fails E_ACCESSDENIED for the rest of the
    // Archicad session. The composition path has no HWND association to lose, but
    // it has the same deferred destruction and the same DWM-still-compositing
    // window, so it gets the same treatment.
    if (context != nullptr) {
        context->SetRenderTargets (0, nullptr, nullptr,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
        context->Flush ();
    }

    impl_->ReleaseOverlay ();
    impl_->swapChain.Release ();
    impl_->deviceD3D11.Release ();
    impl_->device = nullptr;
}

bool DiligentViewportTarget::BeginFrame (Diligent::ITextureView*& rtv, Diligent::ITextureView*& dsv)
{
    rtv = nullptr;
    dsv = nullptr;
    if (impl_ == nullptr)
        return false;

    if (impl_->resizePending) {
        impl_->resizePending = false;
        const uint32_t w = impl_->pendingWidth;
        const uint32_t h = impl_->pendingHeight;
        if (w > 0 && h > 0 && (w != impl_->width || h != impl_->height)) {
            // ⚠️ THE NEW SIZE IS NOT COMMITTED UNTIL THE RESIZE SUCCEEDS. It used
            // to be recorded here, before the call -- so a failed ResizeBuffers
            // left `width`/`height` claiming the size it had NOT reached, the
            // `w != impl_->width` guard above then read as "already there", and
            // the overlay presented at the old size for the rest of the session
            // with nothing ever retrying. A failure that disables its own retry
            // is indistinguishable from the resize tracker not working.
            bool committed = true;
            if (impl_->mode == SurfaceMode::PaletteChild) {
                if (impl_->swapChain != nullptr)
                    impl_->swapChain->Resize (w, h);
            } else if (impl_->compositionSwapChain != nullptr) {
                // ⚠️ THE WRAPPED VIEWS GO FIRST. ResizeBuffers refuses while any
                // outstanding reference to a back buffer exists, and every entry
                // in this map is one -- it fails with DXGI_ERROR_INVALID_CALL and
                // the overlay then presents at the old size forever, which looks
                // like the tracker not working rather than like a failed resize.
                impl_->wrapped.clear ();
                const HRESULT hr = impl_->compositionSwapChain->ResizeBuffers (
                    0, w, h, DXGI_FORMAT_UNKNOWN, 0);
                if (FAILED (hr)) {
                    committed = false;
                    // Logged once per run of failures, not once per frame: a
                    // resize that cannot succeed would otherwise fill the log
                    // faster than anything else in it.
                    if (!impl_->resizeFailed)
                        ArchVizLog ("ArchViz overlay: ResizeBuffers " + HrText (hr) +
                                    " -- retrying on the next frame");
                    impl_->resizeFailed = true;
                } else {
                    impl_->resizeFailed = false;
                }
                std::string depthError;
                if (!impl_->CreateOverlayDepth (depthError))
                    ArchVizLog ("ArchViz overlay: " + depthError);
            }
            if (committed) {
                impl_->width = w;
                impl_->height = h;
            } else {
                // Try again next frame rather than settling at the wrong size.
                impl_->resizePending = true;
            }
        }
    }

    if (impl_->mode == SurfaceMode::PaletteChild) {
        if (impl_->swapChain == nullptr)
            return false;
        rtv = impl_->swapChain->GetCurrentBackBufferRTV ();
        dsv = impl_->swapChain->GetDepthBufferDSV ();
        return rtv != nullptr;
    }

    if (impl_->compositionSwapChain == nullptr || impl_->depthTexture == nullptr)
        return false;

    ID3D11Texture2D* back = nullptr;
    if (FAILED (impl_->compositionSwapChain->GetBuffer (
            0, __uuidof (ID3D11Texture2D), reinterpret_cast<void**> (&back))) ||
        back == nullptr)
        return false;

    auto it = impl_->wrapped.find (back);
    if (it == impl_->wrapped.end ()) {
        Impl::Wrapped entry;
        // RESOURCE_STATE_RENDER_TARGET, not UNKNOWN: we know what it is, and
        // telling Diligent lets it skip a transition it would otherwise guess at.
        impl_->deviceD3D11->CreateTexture2DFromD3DResource (
            back, Diligent::RESOURCE_STATE_RENDER_TARGET, &entry.texture);
        if (entry.texture == nullptr) {
            back->Release ();
            return false;
        }
        entry.view = entry.texture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
        if (entry.view == nullptr) {
            back->Release ();
            return false;
        }
        it = impl_->wrapped.emplace (back, std::move (entry)).first;
    }
    // ⚠️ RELEASED, BUT THE MAP IS KEYED ON THE POINTER. GetBuffer adds a
    // reference; the wrapped Diligent texture holds its own, so the buffer stays
    // alive for as long as the entry does and the raw pointer stays a valid KEY.
    // Keeping this reference instead would be the exact fault PLAT-RE39 was: an
    // extra strong reference deferring destruction past teardown.
    back->Release ();

    rtv = it->second.view;
    dsv = impl_->depthTexture->GetDefaultView (Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    return rtv != nullptr && dsv != nullptr;
}

void DiligentViewportTarget::Present ()
{
    if (impl_ == nullptr)
        return;
    if (impl_->mode == SurfaceMode::PaletteChild) {
        if (impl_->swapChain != nullptr)
            impl_->swapChain->Present (1);
        return;
    }
    if (impl_->compositionSwapChain == nullptr)
        return;
    // ⚠️ SYNC INTERVAL 1, NOT 0, AND A LIVE RUN IS WHY. Present(0) lets DWM pick
    // up a composition buffer mid-update; TransparencyProbe's 2026-08-08 run
    // reported its opaque test bar appearing SPLIT into two offset halves,
    // exactly where it crossed moving geometry. Interval 1 waits for the blank,
    // which is what an overlay wants anyway -- it has no reason to run faster
    // than the compositor can show it.
    // ⚠️ THE RESULT IS RECORDED, NOT DISCARDED. A composition Present can return
    // DXGI_STATUS_OCCLUDED or a device-removed code and simply not show the
    // frame; ignoring it meant a run could report thousands of presented frames
    // while the screen kept an older one, which is exactly the failure being
    // chased and exactly the one this call was hiding.
    const HRESULT hr = impl_->compositionSwapChain->Present (1, 0);
    if (FAILED (hr) || hr != S_OK)
        impl_->lastPresentResult.store (uint32_t (hr), std::memory_order_relaxed);
    if (FAILED (hr))
        impl_->presentFailures.fetch_add (1, std::memory_order_relaxed);
}

// Bound the number of already-rendered frames DXGI may hold before showing one.
//
// ⚠️ THIS IS THE CHEAPEST CANDIDATE FIX FOR THE AFTERIMAGE, and it is
// independent of everything the camera work has been doing: prediction moves
// WHERE the overlay is drawn, and cannot touch WHEN a queued frame is finally
// composited. `frames` of 1 means "never hold more than one".
bool DiligentViewportTarget::SetFrameLatency (uint32_t frames, std::string& error)
{
    if (impl_ == nullptr || impl_->compositionSwapChain == nullptr) {
        error = "no composition swap chain to set the frame latency on";
        return false;
    }
    IDXGISwapChain2* swapChain2 = nullptr;
    HRESULT hr = impl_->compositionSwapChain->QueryInterface (
        __uuidof (IDXGISwapChain2), reinterpret_cast<void**> (&swapChain2));
    if (FAILED (hr) || swapChain2 == nullptr) {
        error = "IDXGISwapChain2 unavailable " + HrText (hr);
        return false;
    }
    hr = swapChain2->SetMaximumFrameLatency (frames < 1 ? 1 : frames);
    swapChain2->Release ();
    if (FAILED (hr)) {
        error = "SetMaximumFrameLatency " + HrText (hr);
        return false;
    }
    impl_->frameLatency.store (frames, std::memory_order_relaxed);
    return true;
}

uint32_t DiligentViewportTarget::FrameLatency () const
{
    return impl_ != nullptr ? impl_->frameLatency.load (std::memory_order_relaxed) : 0;
}

uint32_t DiligentViewportTarget::LastPresentResult () const
{
    return impl_ != nullptr ? impl_->lastPresentResult.load (std::memory_order_relaxed) : 0;
}

uint64_t DiligentViewportTarget::PresentFailures () const
{
    return impl_ != nullptr ? impl_->presentFailures.load (std::memory_order_relaxed) : 0;
}

void DiligentViewportTarget::RequestResize (uint32_t width, uint32_t height)
{
    if (impl_ == nullptr || width == 0 || height == 0)
        return;
    impl_->pendingWidth = width;
    impl_->pendingHeight = height;
    impl_->resizePending = true;
}

uint32_t DiligentViewportTarget::ColorFormat () const { return impl_->colorFormat; }
uint32_t DiligentViewportTarget::DepthFormat () const { return impl_->depthFormat; }
uint32_t DiligentViewportTarget::Width () const { return impl_->width; }
uint32_t DiligentViewportTarget::Height () const { return impl_->height; }
SurfaceMode DiligentViewportTarget::Mode () const { return impl_->mode; }

Diligent::ISwapChain* DiligentViewportTarget::DiligentSwapChain () const
{
    return impl_->swapChain;
}

IDXGISwapChain* DiligentViewportTarget::Dxgi () const
{
    if (impl_->mode == SurfaceMode::Overlay)
        return impl_->compositionSwapChain;
    if (impl_->swapChain == nullptr)
        return nullptr;
    // ⚠️ SCOPED, AND THE BRACES ARE THE WHOLE POINT (PLAT-RE39). RefCntAutoPtr's
    // QueryInterface constructor takes a STRONG reference; left alive past this
    // statement it defers the swap chain's destruction past teardown, the HWND
    // stays taken, and every reopen fails E_ACCESSDENIED. `GetDXGISwapChain` is
    // documented as NOT adding a reference, so the pointer it returns is
    // BORROWED and is valid for as long as `swapChain` is.
    RefCntAutoPtr<Diligent::ISwapChainD3D11> native {impl_->swapChain,
                                                     Diligent::IID_SwapChainD3D11};
    return native != nullptr ? native->GetDXGISwapChain () : nullptr;
}

const float* DiligentViewportTarget::ClearColor () const
{
    return impl_->mode == SurfaceMode::Overlay ? kTransparentClear : kOpaqueClear;
}

}   // namespace archviz
}   // namespace geomsrv
