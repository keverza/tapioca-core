#ifndef EVP_ARCHVIZ_VIEWERSCREENSHOT_HPP
#define EVP_ARCHVIZ_VIEWERSCREENSHOT_HPP

// ArchViz/ViewerScreenshot — a framed high-resolution still of the live viewer,
// saved to Tapioca\Export. Sun study adds its legend and Exposed surfaces adds its
// area table. Exposed surfaces keeps the measured highlight while it stays current.
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
#include "ArchViz/ViewerScreenshotGeometry.hpp"
#include "ArchViz/LayoutReportPage.hpp"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <memory>

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
class Camera;
struct HudState;

// Frames drawn at the new size before the readback: the TAA history is reset
// there and has to accumulate again, or the still is the aliased first frame.
constexpr int kViewerScreenshotSettleFrames = 24;

class ViewerScreenshot final {
  public:
    ViewerScreenshot ();
    ~ViewerScreenshot ();
    ViewerScreenshot (const ViewerScreenshot&) = delete;
    ViewerScreenshot& operator= (const ViewerScreenshot&) = delete;

    void ServiceFramingCamera (Camera& camera, DiligentScene& scene, HudState& state, bool& userHasNavigated);
    // At the top of a frame: the target THIS frame draws into -- `surface`
    // unless a screenshot is being taken. Starts one when the HUD asked.
    DiligentViewportTarget& TargetFor (DiligentViewportTarget& surface, Diligent::IRenderDevice* device,
                                       Diligent::IDeviceContext* context, DiligentScene& scene, HudState& state);
    bool Active () const;
    Camera& CameraFor (Camera& live);
    static bool HidesSurfaceFocus (const HudState& state);
    float EditorDrawScale (const HudState& state) const; // nonzero keeps only the area highlight
    uint32_t FrameIndex (uint32_t presented) const;

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
    std::unique_ptr<DiligentHud> paperLegendHud_;
    std::unique_ptr<Camera> framingCamera_;
    std::unique_ptr<Camera> comparisonCamera_;
    bool framingUserNavigated_ = false;
    bool regionsRead_ = false;
    int framingOrbitAnchor_ = 0;
    Diligent::IRenderDevice* device_ = nullptr; // the frame loop's, borrowed while a shot runs
    float scale_ = 1.0f;
    int frames_ = 0;
    int kind_ = 0; // HudState::shotKind, taken with the request
    ScreenshotPixels crop_;
    size_t panelIndex_ = 0;
    double previousShaded_ = -1;
    bool waitingDay_ = false;
    std::chrono::steady_clock::time_point publishingSince_ {};
    uint64_t sourceStamp_ = 0;
    DiligentViewportTarget* progressSurface_ = nullptr;
    LayoutReportPage report_;
    std::filesystem::path exportFolder_;
    bool reporting_ = false;
    std::shared_ptr<const surfacetakeoff::ShotTable> table_;
};

} // namespace geomsrv::archviz

#endif
