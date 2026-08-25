#include "Palette/PreviewPlanCamera.hpp"

#include <algorithm>
#include <cmath>

namespace evp::previewpanel {
namespace {

constexpr double MinimumExtent = 1.0e-9;
constexpr int RawWheelDelta = 120;
constexpr int MaximumWheelNotches = 8;
constexpr double ZoomPerNotch = 1.2;
constexpr uint64_t DoubleClickMilliseconds = 400;

bool IsFiniteBounds (const PlanBounds& bounds)
{
    return bounds.valid && std::isfinite (bounds.minimumX) && std::isfinite (bounds.minimumY) &&
           std::isfinite (bounds.maximumX) && std::isfinite (bounds.maximumY) && bounds.maximumX >= bounds.minimumX &&
           bounds.maximumY >= bounds.minimumY;
}

} // namespace

void PreviewPlanCamera::SetViewport (double width, double height)
{
    if (!std::isfinite (width) || !std::isfinite (height) || width < 0.0 || height < 0.0)
        return;
    viewportWidth = width;
    viewportHeight = height;
}

void PreviewPlanCamera::SetBounds (const PlanBounds& value)
{
    bounds = IsFiniteBounds (value) ? value : PlanBounds {};
}

bool PreviewPlanCamera::Fit (double margin)
{
    if (!IsFiniteBounds (bounds) || !std::isfinite (margin) || margin < 0.0 || viewportWidth <= 2.0 * margin ||
        viewportHeight <= 2.0 * margin)
        return false;

    const double extentX = std::max (bounds.maximumX - bounds.minimumX, MinimumExtent);
    const double extentY = std::max (bounds.maximumY - bounds.minimumY, MinimumExtent);
    const double fitted =
        std::min ((viewportWidth - 2.0 * margin) / extentX, (viewportHeight - 2.0 * margin) / extentY);
    if (!std::isfinite (fitted) || fitted <= 0.0)
        return false;

    centerX = (bounds.minimumX + bounds.maximumX) * 0.5;
    centerY = (bounds.minimumY + bounds.maximumY) * 0.5;
    scale = std::clamp (fitted, MinimumScale, MaximumScale);
    return true;
}

bool PreviewPlanCamera::ZoomAt (double viewportX, double viewportY, int wheelDelta)
{
    if (wheelDelta == 0 || viewportWidth <= 0.0 || viewportHeight <= 0.0 || !std::isfinite (viewportX) ||
        !std::isfinite (viewportY))
        return false;

    int notches = wheelDelta;
    if (notches >= RawWheelDelta || notches <= -RawWheelDelta)
        notches /= RawWheelDelta;
    notches = std::clamp (notches, -MaximumWheelNotches, MaximumWheelNotches);
    const double nextScale = std::clamp (scale * std::pow (ZoomPerNotch, notches), MinimumScale, MaximumScale);
    if (nextScale == scale)
        return false;

    const double offsetX = viewportX - viewportWidth * 0.5;
    const double offsetY = viewportY - viewportHeight * 0.5;
    const double anchorX = centerX + offsetX / scale;
    const double anchorY = centerY - offsetY / scale;
    centerX = anchorX - offsetX / nextScale;
    centerY = anchorY + offsetY / nextScale;
    scale = nextScale;
    return true;
}

bool PreviewPlanCamera::DoubleClickFit ()
{
    Cancel ();
    return Fit ();
}

PlanPointerAction PreviewPlanCamera::MiddleDown (double viewportX, double viewportY, uint64_t nowMilliseconds)
{
    const bool doubleClick = lastMiddleDownMilliseconds != 0 && nowMilliseconds >= lastMiddleDownMilliseconds &&
                             nowMilliseconds - lastMiddleDownMilliseconds <= DoubleClickMilliseconds;
    lastMiddleDownMilliseconds = doubleClick ? 0 : nowMilliseconds;
    if (doubleClick)
        return DoubleClickFit () ? PlanPointerAction::Fitted : PlanPointerAction::None;
    return BeginPan (viewportX, viewportY) ? PlanPointerAction::CaptureStarted : PlanPointerAction::None;
}

bool PreviewPlanCamera::BeginPan (double viewportX, double viewportY)
{
    if (captured || !std::isfinite (viewportX) || !std::isfinite (viewportY))
        return false;
    lastPointerX = viewportX;
    lastPointerY = viewportY;
    captured = true;
    return true;
}

bool PreviewPlanCamera::PanTo (double viewportX, double viewportY)
{
    if (!captured || !std::isfinite (viewportX) || !std::isfinite (viewportY))
        return false;
    centerX -= (viewportX - lastPointerX) / scale;
    centerY += (viewportY - lastPointerY) / scale;
    lastPointerX = viewportX;
    lastPointerY = viewportY;
    return true;
}

bool PreviewPlanCamera::EndPan ()
{
    if (!captured)
        return false;
    captured = false;
    return true;
}

bool PreviewPlanCamera::Cancel ()
{
    lastMiddleDownMilliseconds = 0;
    return EndPan ();
}

geomsrv::annotation::Transform2D PreviewPlanCamera::Transform () const
{
    geomsrv::annotation::Transform2D transform;
    transform.scaleX = scale;
    transform.scaleY = -scale;
    transform.offX = viewportWidth * 0.5 - centerX * scale;
    transform.offY = viewportHeight * 0.5 + centerY * scale;
    return transform;
}

double PreviewPlanCamera::CenterX () const
{
    return centerX;
}

double PreviewPlanCamera::CenterY () const
{
    return centerY;
}

double PreviewPlanCamera::Scale () const
{
    return scale;
}

double PreviewPlanCamera::ViewportWidth () const
{
    return viewportWidth;
}

double PreviewPlanCamera::ViewportHeight () const
{
    return viewportHeight;
}

const PlanBounds& PreviewPlanCamera::Bounds () const
{
    return bounds;
}

bool PreviewPlanCamera::IsCaptured () const
{
    return captured;
}

} // namespace evp::previewpanel
