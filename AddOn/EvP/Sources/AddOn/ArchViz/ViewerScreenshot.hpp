#ifndef EVP_ARCHVIZ_VIEWERSCREENSHOT_HPP
#define EVP_ARCHVIZ_VIEWERSCREENSHOT_HPP

// ArchViz/ViewerScreenshot — a high-resolution still of the LIVE viewer, with
// the sun study's compact legend in the corner, saved to the logs folder.
//
// ⚠️ NOT StartCapture. That path is headless: it refuses while the viewer
// runs and re-extracts the whole model, which is minutes on the models a sun
// study is run on. This one borrows the running frame loop instead: for a few
// frames the loop draws into an OFFSCREEN target of the chosen size, with the
// same camera, scene and study, lets temporal antialiasing settle there, draws
// only the legend over it, reads it back and goes back to the window. The
// window shows its last presented frame meanwhile.
//
// ⚠️ THE PALETTE VIEWER ONLY. The offscreen target is RGBA8_UNORM_SRGB, which is
// the palette swap chain's format, so every pipeline the scene built for the
// window draws into it unchanged. The overlay's surface is BGRA8_UNORM and
// cannot be clicked anyway (WS_EX_TRANSPARENT); it is refused with the reason.
//
// RENDER THREAD ONLY, like DiligentViewportTarget. The request and the result
// travel through HudState, which only the render thread touches.

#include "ArchViz/DiligentViewportTarget.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv::archviz {

class DiligentHud;
class DiligentScene;
struct HudState;

constexpr float kViewerScreenshotScales[] = { 1.5f, 2.0f, 3.0f };
constexpr int kViewerScreenshotScaleCount = 3;
// D3D11's texture limit; the GPU may refuse smaller, and then says so.
constexpr uint32_t kViewerScreenshotMaxDimension = 16384;
// Frames drawn at the new size before the readback: the TAA history is reset
// there and has to accumulate again, or the still is the aliased first frame.
constexpr int kViewerScreenshotSettleFrames = 24;

inline uint32_t ViewerScreenshotSize (uint32_t windowSize, float scale)
{
    return (std::min) (kViewerScreenshotMaxDimension, uint32_t (std::lround (double (windowSize) * double (scale))));
}

class ViewerScreenshot final {
  public:
    ViewerScreenshot ();
    ~ViewerScreenshot ();
    ViewerScreenshot (const ViewerScreenshot&) = delete;
    ViewerScreenshot& operator= (const ViewerScreenshot&) = delete;

    // At the top of a frame: the target THIS frame draws into -- `surface`
    // unless a screenshot is being taken. Starts one when the HUD asked.
    DiligentViewportTarget& TargetFor (DiligentViewportTarget& surface, Diligent::IRenderDevice* device,
                                       Diligent::IDeviceContext* context, DiligentScene& scene, HudState& state);
    bool Active () const;

    // In the HUD's slot: on a screenshot frame, the legend alone, and true so
    // the panels are not drawn into the image.
    bool DrawLegend (DiligentHud& hud, Diligent::IDeviceContext* context, uint32_t width, uint32_t height,
                     DiligentScene& scene, const HudState& state);

    // After the frame unbound its targets. True when the frame was the
    // screenshot's, which is never presented; on the last one the PNG is
    // written and the window's target is back for the next frame.
    bool Finish (Diligent::IDeviceContext* context, DiligentScene& scene, HudState& state);

    // ⚠️ BEFORE THE DEVICE IS RELEASED, like the surface target.
    void Destroy (Diligent::IDeviceContext* context);

  private:
    std::unique_ptr<DiligentViewportTarget> target_;
    float scale_ = 1.0f;
    int frames_ = 0;
};

} // namespace geomsrv::archviz

#endif
