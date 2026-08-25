#ifndef EVP_PLANOVERLAY_PLANTRANSFORMMATH_HPP
#define EVP_PLANOVERLAY_PLANTRANSFORMMATH_HPP

#include <string>
#include <vector>

namespace geomsrv::planoverlay {

struct PlanPoint {
    double x = 0.0;
    double y = 0.0;
};

struct LogicalSampleRect {
    bool valid = false;
    double physicalWidth = 0.0;
    double physicalHeight = 0.0;
    double dpiScale = 1.0;
    long right = 0;
    long bottom = 0;
};

struct PlanTransform {
    bool valid = false;
    // physicalPixel = matrix * modelMetres + offset
    double xx = 0.0;
    double xy = 0.0;
    double yx = 0.0;
    double yy = 0.0;
    double offsetX = 0.0;
    double offsetY = 0.0;
    double tearPixels = 0.0;
    std::string why;
};

LogicalSampleRect MakeLogicalSampleRect (double physicalWidth, double physicalHeight, double dpiScale);

// Fits and inverts the logical-pixel-to-model observation. A repeated top-left
// that moved by more than a quarter physical pixel rejects the whole observation.
PlanTransform ObservePlanTransform (const LogicalSampleRect& rect, const PlanPoint& topLeft, const PlanPoint& topRight,
                                    const PlanPoint& bottomLeft, const PlanPoint& topLeftAgain);

PlanPoint Project (const PlanTransform& transform, const PlanPoint& modelPoint);
bool RoundedProjectedPixelsChanged (const PlanTransform& painted, const PlanTransform& candidate,
                                    const std::vector<PlanPoint>& geometry);

// Invalid observations never replace the last accepted transform.
bool AdoptPlanTransform (const PlanTransform& observation, PlanTransform& accepted);

} // namespace geomsrv::planoverlay

#endif
