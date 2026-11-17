// ArchViz/TechnicalEdges -- see the header.

#include "ArchViz/TechnicalEdges.hpp"

#include "ArchViz/ColourSpace.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

size_t NearestCreaseTier (float degrees)
{
    size_t best = 0;
    for (size_t k = 1; k < kCreaseTierCount; ++k)
        if (std::fabs (kCreaseTierDegrees[k] - degrees) < std::fabs (kCreaseTierDegrees[best] - degrees))
            best = k;
    return best;
}

TechnicalEdgeStyle TechnicalEdgeStyleFromHud (bool enabled, uint32_t srgbRgba, float pixels, bool showHidden,
                                              float angleDegrees)
{
    TechnicalEdgeStyle style;
    style.enabled = enabled;
    style.rgba[0] = SrgbToLinear (float ((srgbRgba >> 24) & 0xFFu) / 255.0f);
    style.rgba[1] = SrgbToLinear (float ((srgbRgba >> 16) & 0xFFu) / 255.0f);
    style.rgba[2] = SrgbToLinear (float ((srgbRgba >> 8) & 0xFFu) / 255.0f);
    style.rgba[3] = float (srgbRgba & 0xFFu) / 255.0f; // alpha is not encoded
    style.pixels = std::clamp (pixels, 0.5f, 6.0f);
    style.showHidden = showHidden;
    style.maskShift = CreaseTierShift (NearestCreaseTier (angleDegrees));
    return style;
}

} // namespace archviz
} // namespace geomsrv
