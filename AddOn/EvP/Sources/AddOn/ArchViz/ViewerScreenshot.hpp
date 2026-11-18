#ifndef EVP_ARCHVIZ_VIEWERSCREENSHOT_HPP
#define EVP_ARCHVIZ_VIEWERSCREENSHOT_HPP

// ArchViz/ViewerScreenshot — a high-resolution still of the LIVE viewer, with
// the sun study's legend in a white strip under it, saved to Tapioca\Export.
// Exposed surfaces (#193) takes the same still with its area table as the
// strip instead (HudState::shotKind): the scene keeps its material focus and the
// measured highlight, and the request is refused if either moves before readback.
//
// ⚠️ NOT StartCapture. That path is headless: it refuses while the viewer
// runs and re-extracts the whole model, which is minutes on the models a sun
// study is run on. This one borrows the running frame loop instead: for a few
// frames the loop draws into an OFFSCREEN target of the chosen size, with the
// same camera, scene and study and no HUD or gizmo, lets temporal antialiasing
// settle there, reads it back, adds the legend strip and goes back to the window. The
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
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv::archviz {

namespace surfacetakeoff {
struct ShotTable;
}
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

// The size presets as the pages list them: "2x  3840 x 2160".
inline std::vector<std::string> ViewerScreenshotPresetLabels (uint32_t width, uint32_t height)
{
    std::vector<std::string> labels;
    for (const float scale : kViewerScreenshotScales) {
        char label[48] = {};
        std::snprintf (label, sizeof (label), "%gx  %u x %u", double (scale),
                       unsigned (ViewerScreenshotSize (width, scale)), unsigned (ViewerScreenshotSize (height, scale)));
        labels.push_back (label);
    }
    return labels;
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
    // The sun study's still hides the material focus; Exposed surfaces' keeps it
    // and the measured highlight (but never the editor's handles or the HUD).
    bool HidesSurfaceFocus () const;
    bool DrawsSurfaceHighlight () const; // also true with no screenshot running
    float Scale () const;                // the running screenshot's, 0 with none

    // After the frame unbound its targets. True when the frame was the
    // screenshot's, which is never presented and carries no HUD. On the last
    // one the view is read back, the legend is drawn as a white strip of its
    // own under it (DiligentHud::DrawCaptureLegend), and the PNG is written to
    // %LOCALAPPDATA%\Tapioca\Export; the window's target is back next frame.
    bool Finish (Diligent::IDeviceContext* context, DiligentScene& scene, DiligentHud& hud, HudState& state);

    // ⚠️ BEFORE THE DEVICE IS RELEASED, like the surface target.
    void Destroy (Diligent::IDeviceContext* context);

  private:
    std::unique_ptr<DiligentViewportTarget> target_;
    Diligent::IRenderDevice* device_ = nullptr; // the frame loop's, borrowed while a shot runs
    float scale_ = 1.0f;
    int frames_ = 0;
    int kind_ = 0; // HudState::shotKind, taken with the request
    std::shared_ptr<const surfacetakeoff::ShotTable> table_;
};

} // namespace geomsrv::archviz

#endif
