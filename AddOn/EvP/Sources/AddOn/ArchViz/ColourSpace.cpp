#include "ArchViz/ColourSpace.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

float Clamp01 (float v)
{
    // NaN lands on 0: `!(v > 0)` is true for it, which a plain std::max is not.
    if (!(v > 0.0f))
        return 0.0f;
    return (std::min) (v, 1.0f);
}

} // namespace

float SrgbToLinear (float encoded)
{
    const float c = Clamp01 (encoded);
    // The breakpoint is 0.04045 on THIS side and 0.0031308 on the other; they
    // are the same point on the curve seen from its two ends, and swapping them
    // puts a visible kink in the near-blacks.
    return c <= 0.04045f ? c / 12.92f : std::pow ((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb (float linear)
{
    const float c = Clamp01 (linear);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow (c, 1.0f / 2.4f) - 0.055f;
}

} // namespace archviz
} // namespace geomsrv
