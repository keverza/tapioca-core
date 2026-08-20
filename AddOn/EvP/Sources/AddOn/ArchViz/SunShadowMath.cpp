#include "ArchViz/SunShadowMath.hpp"

#include "ArchViz/MatrixMath.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {

SunShadow FitSunShadow (const float boundsMin[3], const float boundsMax[3],
                        const float sunDir[3], uint32_t resolution)
{
    SunShadow out;
    if (boundsMin == nullptr || boundsMax == nullptr || sunDir == nullptr || resolution == 0)
        return out;

    float centre[3];
    float radiusSq = 0.0f;
    for (int k = 0; k < 3; ++k) {
        if (!(boundsMax[k] >= boundsMin[k]))   // catches NaN as well as inverted
            return out;
        centre[k] = 0.5f * (boundsMin[k] + boundsMax[k]);
        const float half = 0.5f * (boundsMax[k] - boundsMin[k]);
        radiusSq += half * half;
    }
    const float radius = std::sqrt (radiusSq);
    if (!(radius > 1e-4f))
        return out;

    float light[3] = {sunDir[0], sunDir[1], sunDir[2]};
    const float lengthSq = light[0] * light[0] + light[1] * light[1] + light[2] * light[2];
    if (!(lengthSq > 1e-12f))
        return out;
    const float inv = 1.0f / std::sqrt (lengthSq);
    light[0] *= inv;
    light[1] *= inv;
    light[2] *= inv;

    // A margin in front of the sphere, so the near plane never clips a surface
    // that is exactly tangent to it. Proportional to the model plus an absolute
    // floor: a 3 m garden shed and a 300 m tower both need one.
    const float margin = radius * 0.05f + 1.0f;

    const float eye[3] = {centre[0] + light[0] * (radius + margin),
                          centre[1] + light[1] * (radius + margin),
                          centre[2] + light[2] * (radius + margin)};

    // ⚠️ Z-UP EXCEPT WHEN THE SUN IS OVERHEAD. LookAtRH already falls back to +X
    // when up and the line of sight are parallel, but that fallback exists to
    // avoid NaN, not to choose a good frame: at local noon it would spin the
    // shadow map's orientation through ninety degrees between one frame and the
    // next as the sun crosses the zenith. Switching to +Y a little before that
    // keeps the frame continuous.
    const bool overhead = std::fabs (light[2]) > 0.99f;
    const float up[3] = {0.0f, overhead ? 1.0f : 0.0f, overhead ? 0.0f : 1.0f};

    float view[16];
    LookAtRH (view, eye, centre, up);

    float proj[16];
    OrthographicRH (proj, -radius, radius, -radius, radius, margin, margin + 2.0f * radius);

    Multiply (out.lightViewProj, view, proj);
    out.texelWorldSize = (2.0f * radius) / float (resolution);
    out.depthRange = 2.0f * radius;
    out.valid = true;
    return out;
}

}   // namespace archviz
}   // namespace geomsrv
