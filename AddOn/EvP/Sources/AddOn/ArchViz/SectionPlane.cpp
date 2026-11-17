// ArchViz/SectionPlane -- see the header.

#include "ArchViz/SectionPlane.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

float SectionPlaneCoordinate (const SectionPlaneState& state, const float boundsMin[3], const float boundsMax[3])
{
    const int a = std::clamp (int (state.axis), 0, 2);
    const float t = std::isfinite (state.position) ? std::clamp (state.position, 0.0f, 1.0f) : 0.5f;
    return boundsMin[a] + t * (boundsMax[a] - boundsMin[a]);
}

void SectionPlaneEquation (const SectionPlaneState& state, const float boundsMin[3], const float boundsMax[3],
                           float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const int a = std::clamp (int (state.axis), 0, 2);
    const float extent = boundsMax[a] - boundsMin[a];
    if (!state.enabled || !(extent > 0.0f) || !std::isfinite (extent))
        return;
    const float at = SectionPlaneCoordinate (state, boundsMin, boundsMax);
    // Unflipped keeps p[a] <= at: normal -axis, w = at. Flipped keeps p[a] >= at.
    const float sign = state.flipped ? 1.0f : -1.0f;
    out[a] = sign;
    out[3] = -sign * at;
}

const char* SectionAxisName (SectionAxis axis)
{
    switch (axis) {
        case SectionAxis::X:
            return "X";
        case SectionAxis::Y:
            return "Y";
        case SectionAxis::Z:
            return "Z";
    }
    return "?";
}

} // namespace archviz
} // namespace geomsrv
