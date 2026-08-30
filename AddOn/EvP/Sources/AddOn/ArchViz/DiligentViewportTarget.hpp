#ifndef EVP_ARCHVIZ_DILIGENTVIEWPORTTARGET_HPP
#define EVP_ARCHVIZ_DILIGENTVIEWPORTTARGET_HPP

// ArchViz/DiligentViewportTarget — WHERE the viewport's frames go, so that the
// frame loop does not have to know.
//
// There are three surfaces and they are not variations on one thing:
//
//   PaletteChild  a Diligent HWND swap chain on the DG UserItem. Opaque, its own
//                 depth buffer, `ISwapChain::Present`. This is what the viewport
//                 has always used.
//   Overlay       a DirectComposition visual over Archicad's own 3D view
//                 (PLAT-RE37). NO HWND swap chain at all: the chain is created
//                 with `CreateSwapChainForComposition`, has premultiplied alpha,
//                 is presented through raw DXGI, and its back buffers are wrapped
//                 into Diligent textures we did not create.
//   Offscreen     an owned colour/depth pair with no HWND and no Present. The
//                 final LDR colour texture is staged and encoded as PNG.
//
// ⚠️ ONE INTERFACE RATHER THAN TWO FRAME LOOPS, AND THAT IS THE WHOLE REASON THIS
// FILE EXISTS. A second copy of the loop would have to keep the camera, the
// picking, the shadow pass, the HUD and the teardown ordering in step with the
// first, and the two would diverge on the first fix applied to one of them --
// which is exactly what §7 caveat 8 says about running two renderers in
// parallel, one level down.
//
// ⚠️ THE OVERLAY PATH IS THE ONE PROBE 1b MEASURED. `AttachToD3D11Device` is not
// needed here (we own the device), but `CreateTexture2DFromD3DResource` on a
// composition back buffer is exactly the call 1b proved works, and the
// premultiplied compositing it proved correct band-for-band against raw D3D11 is
// what this depends on. See archive/docs/render-engine-decision.md §6.
//
// ⚠️ RENDER THREAD ONLY. It owns Diligent and D3D11 objects and drives the
// immediate context. The HWND arrives from the main thread once, by value.

#include <cstdint>
#include <memory>
#include <string>

struct IDXGISwapChain;

namespace Diligent {
struct IDeviceContext;
struct IEngineFactoryD3D11;
struct IRenderDevice;
struct ISwapChain;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

enum class SurfaceMode : uint8_t {
    PaletteChild = 0,
    Overlay = 1,
    Offscreen = 2,
};

class DiligentViewportTarget final {
  public:
    DiligentViewportTarget ();
    ~DiligentViewportTarget ();
    DiligentViewportTarget (const DiligentViewportTarget&) = delete;
    DiligentViewportTarget& operator= (const DiligentViewportTarget&) = delete;

    // `error` carries the reason on false -- an alert cannot, and neither can a
    // black rectangle.
    bool Create (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                 Diligent::IEngineFactoryD3D11* factory, SurfaceMode mode, void* hwnd, uint32_t width, uint32_t height,
                 std::string& error);

    // ⚠️ CALL IT BEFORE THE DEVICE IS RELEASED, and pass the context: the flip
    // model requires the immediate context to have let go of the back buffers
    // before the swap chain is destroyed, and this is where that happens for the
    // composition path exactly as `releaseEverything` does it for the HWND one.
    void Destroy (Diligent::IDeviceContext* context);

    // The views for THIS frame.
    //
    // ⚠️ THE COLOUR VIEW MUST BE FETCHED EVERY FRAME AND MAY NOT BE CACHED. A
    // flip-model chain rotates its buffers, so buffer 0 is a different texture
    // after every Present; a cached view draws into a buffer that is not the one
    // about to be shown, which reads as the viewport running at half rate or
    // tearing rather than as a stale binding.
    bool BeginFrame (Diligent::ITextureView*& rtv, Diligent::ITextureView*& dsv);
    void Present ();

    // Offscreen mode only. The caller must unbind the colour target before this
    // call; the method copies through a staging texture and returns PNG bytes.
    bool CapturePng (Diligent::IDeviceContext* context, std::string& png, std::string& error);

    // Applied at the next BeginFrame, not here: resizing between a bind and a
    // draw destroys the texture the context is holding.
    void RequestResize (uint32_t width, uint32_t height);

    uint32_t ColorFormat () const; // raw Diligent::TEXTURE_FORMAT
    uint32_t DepthFormat () const;
    uint32_t Width () const;
    uint32_t Height () const;
    SurfaceMode Mode () const;

    // ⚠️ NULL IN OVERLAY MODE, and every caller has to handle that. The clear
    // A/B, the HUD's format pair and the present counter all reach for an
    // `ISwapChain` today; in overlay mode there is not one, because a
    // composition chain is not presented to a window.
    Diligent::ISwapChain* DiligentSwapChain () const;
    IDXGISwapChain* Dxgi () const;

    // ---- present accounting and frame latency (PLAT-RE99) ------------------
    //
    // ⚠️ FRAME LATENCY IS THE ONE THING PREDICTION CANNOT REACH. Everything the
    // camera work has done moves WHERE the overlay is drawn; none of it affects
    // WHEN an already-rendered frame is finally composited. DXGI's default queue
    // depth is 3, so up to three finished overlay frames can be waiting while
    // Archicad has moved on -- which is a lingering afterimage that the desync
    // measurement cannot see at all, because that measurement timestamps
    // SUBMISSION (it logs after Present returns), not display.
    //
    // The composition chain uses IDXGISwapChain2. The palette uses its dedicated
    // D3D11 device's IDXGIDevice1 control, so both interactive surfaces can keep
    // the queue at one frame without changing presentation mode.
    bool SetFrameLatency (uint32_t frames, std::string& error);
    uint32_t FrameLatency () const; // 0 = never set, i.e. DXGI's default 3

    // The last non-S_OK Present result, and how many outright failed. Present
    // can return DXGI_STATUS_OCCLUDED and simply not show the frame; discarding
    // that let a run report thousands of presented frames while the screen kept
    // an older one.
    uint32_t LastPresentResult () const;
    uint64_t PresentFailures () const;

    // What the frame is cleared to. ⚠️ THE OVERLAY CLEARS TO FULLY TRANSPARENT
    // BLACK (0,0,0,0), NOT TO A COLOUR WITH ZERO ALPHA. The composition chain is
    // PREMULTIPLIED, so a clear of (0,0,1,0) is "blue at zero coverage", which is
    // not representable and comes back as an additive blue haze over Archicad's
    // window. Premultiplied transparency is exactly one value.
    const float* ClearColor () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
