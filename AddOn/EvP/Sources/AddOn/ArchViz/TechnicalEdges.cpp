// ArchViz/TechnicalEdges -- see the header.

#include "ArchViz/TechnicalEdges.hpp"

#include "ArchViz/ColourSpace.hpp"
#include "ArchViz/MatrixMath.hpp"

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

TechnicalEdgeLengths MeasureTechnicalEdges (const std::vector<float>& positions, const std::vector<uint32_t>& indices,
                                            const std::vector<uint32_t>& wireMasks)
{
    TechnicalEdgeLengths lengths;
    // Edge k of a triangle is opposite corner k, as the hull shader and BuildCreaseEdgeMasks read it.
    static constexpr int kEdgeCorners[3][2] = { { 1, 2 }, { 2, 0 }, { 0, 1 } };
    const size_t vertexCount = positions.size () / 3;
    const size_t triCount = std::min (indices.size () / 3, wireMasks.size ());
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t mask = wireMasks[t];
        if ((mask >> CreaseTierShift (0)) == 0)
            continue;
        for (int edge = 0; edge < 3; ++edge) {
            const uint32_t a = indices[t * 3 + kEdgeCorners[edge][0]];
            const uint32_t b = indices[t * 3 + kEdgeCorners[edge][1]];
            if (a >= vertexCount || b >= vertexCount)
                continue;
            const float dx = positions[size_t (b) * 3] - positions[size_t (a) * 3];
            const float dy = positions[size_t (b) * 3 + 1] - positions[size_t (a) * 3 + 1];
            const float dz = positions[size_t (b) * 3 + 2] - positions[size_t (a) * 3 + 2];
            const float length = std::sqrt (dx * dx + dy * dy + dz * dz);
            for (size_t tier = 0; tier < kCreaseTierCount; ++tier)
                if (((mask >> CreaseTierShift (tier)) & (1u << edge)) != 0)
                    lengths.metres[tier] += length;
        }
    }
    return lengths;
}

float TechnicalEdgeSpacingPixels (const float boundsMin[3], const float boundsMax[3], float edgeMetres,
                                  float pixelsPerMetre)
{
    if (!(edgeMetres > 0.0f) || !(pixelsPerMetre > 0.0f))
        return 0.0f;
    float size[3];
    for (int axis = 0; axis < 3; ++axis)
        size[axis] = std::max (boundsMax[axis] - boundsMin[axis], 0.0f);
    std::sort (size, size + 3);
    // The two largest extents: a slat screen's face, a wall's elevation, a slab's plan.
    const float area = size[2] * size[1];
    return 4.0f * area / edgeMetres * pixelsPerMetre;
}

float PixelsPerMetreAt (const float viewProj[16], const float point[3], float viewportWidth, float viewportHeight)
{
    constexpr float kStep = 0.1f;
    const float centre[4] = { point[0], point[1], point[2], 1.0f };
    float c[4];
    TransformPoint (c, centre, viewProj);
    if (!(c[3] > 1.0e-4f))
        return 0.0f;
    float best = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        float moved[4] = { point[0], point[1], point[2], 1.0f };
        moved[axis] += kStep;
        float m[4];
        TransformPoint (m, moved, viewProj);
        if (!(m[3] > 1.0e-4f))
            continue;
        const float dx = 0.5f * viewportWidth * (m[0] / m[3] - c[0] / c[3]);
        const float dy = 0.5f * viewportHeight * (m[1] / m[3] - c[1] / c[3]);
        best = std::max (best, std::sqrt (dx * dx + dy * dy) / kStep);
    }
    return best;
}

float TechnicalEdgeFade (float spacingPixels)
{
    const float t = std::clamp (
        (spacingPixels - kTechnicalEdgeFadeTo) / (kTechnicalEdgeFadeFrom - kTechnicalEdgeFadeTo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace archviz
} // namespace geomsrv
