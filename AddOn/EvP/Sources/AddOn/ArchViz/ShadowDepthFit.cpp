#include "ArchViz/ShadowDepthFit.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>

namespace geomsrv {
namespace archviz {

bool FitShadowDepthRange (const float view[16], const float boxMin[3], const float boxMax[3], float& nearDepth,
                          float& farDepth)
{
    if (view == nullptr || boxMin == nullptr || boxMax == nullptr || !(nearDepth > 0.0f) || !(farDepth > nearDepth))
        return false;
    float lo = std::numeric_limits<float>::max ();
    float hi = -std::numeric_limits<float>::max ();
    for (int corner = 0; corner < 8; ++corner) {
        const float x = (corner & 1) ? boxMax[0] : boxMin[0];
        const float y = (corner & 2) ? boxMax[1] : boxMin[1];
        const float z = (corner & 4) ? boxMax[2] : boxMin[2];
        // Row-vector order: view-space z is the third column; the camera looks down -z, so
        // depth is its negation.
        const float depth = -(x * view[2] + y * view[6] + z * view[10] + view[14]);
        lo = (std::min) (lo, depth);
        hi = (std::max) (hi, depth);
    }
    if (!std::isfinite (lo) || !std::isfinite (hi) || hi <= nearDepth)
        return false;
    constexpr float kStepsPerOctave = 4.0f;
    const float fittedFar = std::exp2 (std::ceil (std::log2 (hi) * kStepsPerOctave) / kStepsPerOctave);
    const float fittedNear =
        lo > nearDepth ? std::exp2 (std::floor (std::log2 (lo) * kStepsPerOctave) / kStepsPerOctave) : nearDepth;
    const float newFar = (std::min) (farDepth, fittedFar);
    const float newNear = (std::max) (nearDepth, fittedNear);
    if (!(newFar > newNear) || (newFar >= farDepth && newNear <= nearDepth))
        return false;
    nearDepth = newNear;
    farDepth = newFar;
    return true;
}

bool VisibleShadowDepthRange (const float projection[16], float nearNdc, float farNdc, float margin, float& nearDepth,
                              float& farDepth)
{
    if (projection == nullptr || !(nearNdc <= farNdc) || !(margin >= 1.0f))
        return false;
    // Row-vector order: a view-space point (0, 0, -d, 1) gives z_clip = -d*P[10] + P[14] and
    // w = -d*P[11] + P[15]; solved for d at a given NDC depth.
    const auto depthAt = [projection] (float ndc) {
        const float denominator = projection[10] - ndc * projection[11];
        return std::abs (denominator) > 1e-12f ? (projection[14] - ndc * projection[15]) / denominator : -1.0f;
    };
    float a = depthAt (nearNdc), b = depthAt (farNdc);
    if (a > b)
        std::swap (a, b); // a reversed-Z projection
    if (!(a > 0.0f) || !std::isfinite (b))
        return false;
    constexpr float kStepsPerOctave = 4.0f;
    const float lo = std::exp2 (std::floor (std::log2 (a / margin) * kStepsPerOctave) / kStepsPerOctave);
    const float hi = std::exp2 (std::ceil (std::log2 (b * margin) * kStepsPerOctave) / kStepsPerOctave);
    if (!(hi > lo))
        return false;
    nearDepth = lo;
    farDepth = hi;
    return true;
}

} // namespace archviz
} // namespace geomsrv
