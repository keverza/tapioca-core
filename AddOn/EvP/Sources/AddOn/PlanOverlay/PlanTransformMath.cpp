#include "PlanOverlay/PlanTransformMath.hpp"

#include <cmath>

namespace geomsrv::planoverlay {
namespace {

constexpr double kMinAxis = 1e-12;
constexpr double kMaxSquareness = 0.01;
constexpr double kMaxScaleSkew = 0.02;
constexpr double kMaxTearPixels = 0.25;

bool Finite (const PlanPoint& point)
{
    return std::isfinite (point.x) && std::isfinite (point.y);
}

double Length (double x, double y)
{
    return std::hypot (x, y);
}

} // namespace

LogicalSampleRect MakeLogicalSampleRect (double physicalWidth, double physicalHeight, double dpiScale)
{
    LogicalSampleRect rect;
    rect.physicalWidth = physicalWidth;
    rect.physicalHeight = physicalHeight;
    rect.dpiScale = dpiScale;
    if (!std::isfinite (physicalWidth) || !std::isfinite (physicalHeight) || !std::isfinite (dpiScale) ||
        physicalWidth < 2.0 || physicalHeight < 2.0 || dpiScale <= 0.0)
        return rect;

    rect.right = std::lround (physicalWidth / dpiScale);
    rect.bottom = std::lround (physicalHeight / dpiScale);
    rect.valid = rect.right >= 2 && rect.bottom >= 2;
    return rect;
}

PlanTransform ObservePlanTransform (const LogicalSampleRect& rect, const PlanPoint& topLeft, const PlanPoint& topRight,
                                    const PlanPoint& bottomLeft, const PlanPoint& topLeftAgain)
{
    PlanTransform transform;
    if (!rect.valid || !Finite (topLeft) || !Finite (topRight) || !Finite (bottomLeft) || !Finite (topLeftAgain)) {
        transform.why = "invalid dimensions or non-finite samples";
        return transform;
    }

    const double rightX = (topRight.x - topLeft.x) / double (rect.right);
    const double rightY = (topRight.y - topLeft.y) / double (rect.right);
    const double downX = (bottomLeft.x - topLeft.x) / double (rect.bottom);
    const double downY = (bottomLeft.y - topLeft.y) / double (rect.bottom);
    const double rightLength = Length (rightX, rightY);
    const double downLength = Length (downX, downY);
    const double determinant = rightX * downY - downX * rightY;
    if (!(rightLength > kMinAxis) || !(downLength > kMinAxis)) {
        transform.why = "degenerate pixel-to-model mapping";
        return transform;
    }

    const double squareness = std::fabs ((rightX * downX + rightY * downY) / (rightLength * downLength));
    if (squareness > kMaxSquareness) {
        transform.why = "sheared pixel-to-model mapping";
        return transform;
    }
    if (determinant == 0.0 || !std::isfinite (determinant)) {
        transform.why = "degenerate pixel-to-model mapping";
        return transform;
    }
    if (std::fabs (rightLength / downLength - 1.0) > kMaxScaleSkew) {
        transform.why = "anisotropic pixel-to-model mapping";
        return transform;
    }
    // Screen down is clockwise from screen right in model space. A positive
    // determinant is therefore a reflected plan, not an Archicad plan view.
    if (determinant > 0.0) {
        transform.why = "mirrored pixel-to-model mapping";
        return transform;
    }

    // Invert [right down], first in logical pixels and then in physical pixels.
    const double inv00 = downY / determinant;
    const double inv01 = -downX / determinant;
    const double inv10 = -rightY / determinant;
    const double inv11 = rightX / determinant;

    const double driftX = topLeftAgain.x - topLeft.x;
    const double driftY = topLeftAgain.y - topLeft.y;
    const double tearLogicalX = inv00 * driftX + inv01 * driftY;
    const double tearLogicalY = inv10 * driftX + inv11 * driftY;
    transform.tearPixels = rect.dpiScale * Length (tearLogicalX, tearLogicalY);
    if (transform.tearPixels > kMaxTearPixels) {
        transform.why = "the repeated top-left sample moved while observing the view";
        return transform;
    }

    transform.xx = rect.dpiScale * inv00;
    transform.xy = rect.dpiScale * inv01;
    transform.yx = rect.dpiScale * inv10;
    transform.yy = rect.dpiScale * inv11;
    transform.offsetX = -(transform.xx * topLeft.x + transform.xy * topLeft.y);
    transform.offsetY = -(transform.yx * topLeft.x + transform.yy * topLeft.y);
    transform.valid = true;
    return transform;
}

PlanPoint Project (const PlanTransform& transform, const PlanPoint& modelPoint)
{
    return { transform.offsetX + transform.xx * modelPoint.x + transform.xy * modelPoint.y,
             transform.offsetY + transform.yx * modelPoint.x + transform.yy * modelPoint.y };
}

bool RoundedProjectedPixelsChanged (const PlanTransform& painted, const PlanTransform& candidate,
                                    const std::vector<PlanPoint>& geometry)
{
    if (!candidate.valid)
        return false;
    if (!painted.valid)
        return !geometry.empty ();
    for (const PlanPoint& point : geometry) {
        const PlanPoint before = Project (painted, point);
        const PlanPoint after = Project (candidate, point);
        if (std::lround (before.x) != std::lround (after.x) || std::lround (before.y) != std::lround (after.y))
            return true;
    }
    return false;
}

bool AdoptPlanTransform (const PlanTransform& observation, PlanTransform& accepted)
{
    if (!observation.valid)
        return false;
    accepted = observation;
    return true;
}

} // namespace geomsrv::planoverlay
