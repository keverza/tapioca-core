// The overlay composition: which layers are drawn, into whose depth, in what
// order. Split out of `InjectionRenderer` along the seam its OVERSIZED entry
// named, and the entry is gone because the debt is paid rather than renewed.
//
// ⚠️ THE ORDER IS THE POLICY AND IT LIVES IN ONE PLACE NOW.
// `InjectionRenderer` keeps the arming, the latching and the Present
// bookkeeping; everything about WHAT the overlay is made of is here. The layer
// registry belongs on this side of the seam.
//
// THREAD SAFETY. `Compose` runs on Archicad's render thread from inside a
// detour, under `ScopedInjectionGuard`, with the caller's pipeline state already
// saved by `ScopedPipelineState`. It restores nothing itself.

#ifndef GEOMSRV_ARCHVIZ_DXGI_OVERLAYCOMPOSER_HPP
#define GEOMSRV_ARCHVIZ_DXGI_OVERLAYCOMPOSER_HPP

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace overlaycompose {

// RENDER THREAD. Render the host occluder, then compose every enabled overlay
// layer into its depth buffer. `interpretation` is the camera variant the census
// selected; `depthView` is the injection's own depth view, used when there is no
// host snapshot to occlude against yet.
void Compose (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation,
              ID3D11RenderTargetView* targetView, ID3D11DepthStencilView* depthView);

// The device objects of what `Compose` draws -- the host overlays and the caller's
// layers -- once no Present can reach them (`injection::Shutdown`). What the caller
// published is kept for the next session.
void Shutdown ();

struct Stats {
    uint64_t passes = 0;
    // ⚠️ HOW OFTEN THE DEPTH VIEW WAS THE WRONG SIZE FOR THE
    // SURFACE BEING DRAWN INTO. D3D11 requires every bound render target and the
    // depth-stencil to have IDENTICAL dimensions; when they do not, the runtime
    // refuses the binding and the draw does not land where the caller believes.
    // The overlay composes into the swap-chain BACK BUFFER at Present, while the
    // host occluder's private depth is a copy of ARCHICAD'S SCENE DEPTH, which is
    // sized to the 3D view. Those agree only when the 3D view happens to fill the
    // window -- which is why the overlay was reported visible in full screen and
    // invisible in a window, with the log truthfully saying it had drawn.
    // Counts a sample-count disagreement too (`DepthFitsTarget`): the same refusal.
    uint64_t sizeMismatches = 0;
    // Passes at which the user had hidden the overlay (OverlayVisibility.hpp): the HUD
    // alone drawn, no occluder rendered -- counted, not a silent return (§7).
    uint64_t hidden = 0;
    uint32_t targetWidth = 0, targetHeight = 0;
    uint32_t depthWidth = 0, depthHeight = 0;
    // ⚠️ AND THE SAMPLE COUNTS (2026-10-01): D3D11 refuses a render target and a depth
    // whose sample counts differ just as it refuses a pixel of width, and the size alone
    // read equal while the user saw nothing. 0 when the view could not be asked.
    uint32_t targetSamples = 0, depthSamples = 0;
};
Stats GetStats ();

// ⚠️ WHETHER A DEPTH CAN BE BOUND BESIDE THE TARGET: D3D11 refuses a binding whose
// render target and depth-stencil differ in size by one pixel OR IN SAMPLE COUNT, and
// then nothing composed lands while every counter rises (2026-10-01, suspected: Archicad's
// scene depth multisampled, the back buffer not). Pure, so tests/cpp states it.
inline bool DepthFitsTarget (uint32_t targetWidth, uint32_t targetHeight, uint32_t targetSamples, uint32_t depthWidth,
                             uint32_t depthHeight, uint32_t depthSamples)
{
    return targetWidth == depthWidth && targetHeight == depthHeight && targetSamples == depthSamples;
}

} // namespace overlaycompose
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
