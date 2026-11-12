#include "ArchViz/SunPathLabels.hpp"

#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunPathCompass.hpp"

#include <cmath>

namespace geomsrv::archviz {

namespace {

bool Project (const double point[3], const float viewProj[16], uint32_t width, uint32_t height, ScreenPoint& screen,
              float& depth)
{
    const float input[4] = { float (point[0]), float (point[1]), float (point[2]), 1.0f };
    float clip[4];
    TransformPoint (clip, input, viewProj);
    if (!std::isfinite (clip[0]) || !std::isfinite (clip[1]) || !std::isfinite (clip[2]) || !std::isfinite (clip[3]) ||
        clip[3] <= 1.0e-6f || clip[2] < 0.0f || clip[2] > clip[3])
        return false;
    screen.x = (clip[0] / clip[3] * 0.5f + 0.5f) * float (width);
    screen.y = (0.5f - clip[1] / clip[3] * 0.5f) * float (height);
    depth = clip[2] / clip[3]; // the depth buffer's own NDC value, as TraceAnnotationLayer writes it
    return std::isfinite (screen.x) && std::isfinite (screen.y);
}

} // namespace

std::vector<ScreenLabel> ProjectSunPathLabels (const std::vector<SunPathLabelSpec>& specs, const float viewProj[16],
                                               uint32_t width, uint32_t height, float dpiScale)
{
    std::vector<ScreenLabel> labels;
    if (viewProj == nullptr || width == 0 || height == 0)
        return labels;
    const float dpi = std::isfinite (dpiScale) && dpiScale > 0.0f ? dpiScale : 1.0f;
    labels.reserve (specs.size ());
    for (const SunPathLabelSpec& spec : specs) {
        ScreenPoint at;
        float depth = 1.0f;
        if (spec.text.empty () || !Project (spec.anchor, viewProj, width, height, at, depth))
            continue;
        ScreenLabel label;
        label.text = spec.text;
        label.rgba = spec.rgba;
        label.haloRgba = spec.haloRgba;
        label.haloWidthPixels = spec.haloRgba != 0 ? 1.25f : 0.0f;
        label.fontSize = spec.sizePoints * dpi;
        label.centered = spec.centered;
        label.backgroundPanel = spec.panel;
        label.allowOverlap = true;
        label.hideWhenOccluded = true;
        label.depthAnchor = at;
        label.anchorDepth = depth;
        if (spec.tangent[0] != 0.0 || spec.tangent[1] != 0.0 || spec.tangent[2] != 0.0) {
            double before[3], after[3];
            for (int axis = 0; axis < 3; ++axis) {
                before[axis] = spec.anchor[axis] - spec.tangent[axis];
                after[axis] = spec.anchor[axis] + spec.tangent[axis];
            }
            ScreenPoint a, b;
            float ignored = 0.0f;
            if (!Project (before, viewProj, width, height, a, ignored) ||
                !Project (after, viewProj, width, height, b, ignored) || std::hypot (b.x - a.x, b.y - a.y) <= 1.0e-4f)
                continue; // seen edge-on: no direction to run along
            label.rotationRadians = float (SunPathCompassAngle (b.x - a.x, b.y - a.y));
        }
        // A centred label hangs from its anchor's top; lift it by half its
        // height so it sits ON the anchor, which is also its rotation pivot.
        const float lift = spec.centered ? 0.36f * label.fontSize : 0.0f;
        label.anchor.x = at.x + spec.offsetX * dpi;
        label.anchor.y = at.y + spec.offsetY * dpi - lift;
        labels.push_back (std::move (label));
    }
    return labels;
}

} // namespace geomsrv::archviz
