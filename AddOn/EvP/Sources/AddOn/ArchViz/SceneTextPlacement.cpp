#include "ArchViz/SceneTextPlacement.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz {

SceneTextPlacement ResolveSceneTextPlacement (const SceneTextBounds& bounds, float surfaceWidth, float surfaceHeight,
                                              float edgeInset, float overlapGap,
                                              const std::vector<SceneTextBounds>& occupiedBounds)
{
    SceneTextPlacement result;
    result.bounds = bounds;
    if (!std::isfinite (bounds.left) || !std::isfinite (bounds.top) || !std::isfinite (bounds.right) ||
        !std::isfinite (bounds.bottom) || !std::isfinite (surfaceWidth) || !std::isfinite (surfaceHeight) ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top || surfaceWidth <= 0.0f || surfaceHeight <= 0.0f)
        return result;

    if (bounds.right <= 0.0f || bounds.left >= surfaceWidth || bounds.bottom <= 0.0f || bounds.top >= surfaceHeight)
        return result;

    const float inset = std::clamp (edgeInset, 0.0f, 0.5f * (std::min) (surfaceWidth, surfaceHeight));
    const float availableWidth = surfaceWidth - 2.0f * inset;
    const float availableHeight = surfaceHeight - 2.0f * inset;
    if (bounds.right - bounds.left <= availableWidth) {
        if (bounds.left < inset)
            result.offsetX = inset - bounds.left;
        else if (bounds.right > surfaceWidth - inset)
            result.offsetX = surfaceWidth - inset - bounds.right;
    }
    if (bounds.bottom - bounds.top <= availableHeight) {
        if (bounds.top < inset)
            result.offsetY = inset - bounds.top;
        else if (bounds.bottom > surfaceHeight - inset)
            result.offsetY = surfaceHeight - inset - bounds.bottom;
    }
    result.bounds.left += result.offsetX;
    result.bounds.right += result.offsetX;
    result.bounds.top += result.offsetY;
    result.bounds.bottom += result.offsetY;

    const float gap = (std::max) (overlapGap, 0.0f);
    for (const SceneTextBounds& occupied : occupiedBounds) {
        if (result.bounds.left < occupied.right + gap && result.bounds.right + gap > occupied.left &&
            result.bounds.top < occupied.bottom + gap && result.bounds.bottom + gap > occupied.top)
            return result;
    }
    result.accepted = true;
    return result;
}

} // namespace geomsrv::archviz
