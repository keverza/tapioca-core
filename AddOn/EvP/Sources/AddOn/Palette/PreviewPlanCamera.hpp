#ifndef EVP_PALETTE_PREVIEWPLANCAMERA_HPP
#define EVP_PALETTE_PREVIEWPLANCAMERA_HPP

#include "Annotation/DrawList.hpp"

#include <cstdint>

namespace evp::previewpanel {

struct PlanBounds {
    double minimumX = 0.0;
    double minimumY = 0.0;
    double maximumX = 0.0;
    double maximumY = 0.0;
    bool valid = false;
};

enum class PlanPointerAction { None, CaptureStarted, Fitted };

// DevKit-free plan camera. Model Y points up; viewport Y points down.
class PreviewPlanCamera {
  public:
    static constexpr double MinimumScale = 1.0e-6;
    static constexpr double MaximumScale = 1.0e6;

    void SetViewport (double width, double height);
    void SetBounds (const PlanBounds& bounds);
    bool Fit (double margin = 12.0);
    bool ZoomAt (double viewportX, double viewportY, int wheelDelta);

    bool DoubleClickFit ();
    PlanPointerAction MiddleDown (double viewportX, double viewportY, uint64_t nowMilliseconds);
    bool BeginPan (double viewportX, double viewportY);
    bool PanTo (double viewportX, double viewportY);
    bool EndPan ();
    bool Cancel ();

    geomsrv::annotation::Transform2D Transform () const;
    double CenterX () const;
    double CenterY () const;
    double Scale () const;
    double ViewportWidth () const;
    double ViewportHeight () const;
    const PlanBounds& Bounds () const;
    bool IsCaptured () const;

  private:
    double centerX = 0.0;
    double centerY = 0.0;
    double scale = 1.0;
    double viewportWidth = 0.0;
    double viewportHeight = 0.0;
    PlanBounds bounds;
    double lastPointerX = 0.0;
    double lastPointerY = 0.0;
    uint64_t lastMiddleDownMilliseconds = 0;
    bool captured = false;
};

} // namespace evp::previewpanel

#endif
