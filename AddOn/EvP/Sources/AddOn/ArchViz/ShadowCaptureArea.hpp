#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace geomsrv::archviz {

struct ShadowCaptureArea {
    double measured = 0, shaded = 0, missing = 0;
    bool valid = false;
};

// Sample area, not screen coverage. Empty atlas texels and gutters never
// contribute; a receiver with no mapped texel is reported separately.
template <class TexelOf>
ShadowCaptureArea MeasureShadowArea (const std::vector<double>& areas, const std::vector<uint32_t>& masks, size_t plane,
                                     unsigned step, TexelOf texelOf)
{
    ShadowCaptureArea result;
    if (!plane || masks.size () / plane <= step / 32)
        return result;
    for (size_t i = 0; i < areas.size (); ++i) {
        const double a = areas[i];
        if (!std::isfinite (a) || a < 0)
            return {};
        const int64_t texel = texelOf (i);
        if (texel < 0 || size_t (texel) >= plane) {
            result.missing += a;
            continue;
        }
        result.measured += a;
        if (!(masks[size_t (step / 32) * plane + size_t (texel)] & (1u << (step % 32))))
            result.shaded += a;
    }
    result.valid = true;
    return result;
}

} // namespace geomsrv::archviz
