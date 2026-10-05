#include "ArchViz/HudMassingLabels.hpp"
#include "ArchViz/AnnotationScreenLayout.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::hudmassingrules {
std::optional<ScreenLabel> PlaceDiagramLabel (ProjectedDrawList& occupied, const DiagramLabel& input,
                                              ScreenPoint minimum, ScreenPoint maximum, float fontSize,
                                              const ScreenTextMeasure& measure)
{
    ScreenTextExtent extent;
    for (const auto point : { input.anchor, minimum, maximum })
        if (!std::isfinite (point.x) || !std::isfinite (point.y))
            return {};
    if (!measure || !measure (input.text, fontSize, extent) || !std::isfinite (extent.width) ||
        !std::isfinite (extent.height) || extent.width <= 0 || extent.height <= 0 || !std::isfinite (fontSize) ||
        fontSize <= 0 || occupied.lines.size () > 16000)
        return {};
    const float length = std::hypot (input.direction.x, input.direction.y);
    if (!std::isfinite (length) || length < 1e-5f)
        return {};
    ScreenPoint along { input.direction.x / length, input.direction.y / length };
    // Left-to-right, with vertical labels reading bottom-to-top, independent of ring winding.
    if (along.x < -1e-5f || (std::abs (along.x) <= 1e-5f && along.y > 0)) {
        along.x = -along.x;
        along.y = -along.y;
    }
    const ScreenPoint across { -along.y, along.x };
    const float rotation = std::atan2 (along.y, along.x);
    const float scale = fontSize / 13;
    const float gap = 5 * scale;
    const float halfX = (std::abs (along.x) * extent.width + std::abs (along.y) * extent.height) / 2;
    const float halfY = (std::abs (along.y) * extent.width + std::abs (along.x) * extent.height) / 2;
    for (int lane = 0; lane < 4; ++lane)
        for (float shift : { 0.0f, -0.6f, 0.6f })
            for (int side : { -1, 1 }) {
                const float offset = side * (extent.height / 2 + gap + 2 * scale + lane * (extent.height + gap));
                const ScreenPoint center { input.anchor.x + across.x * offset + along.x * shift * extent.width,
                                           input.anchor.y + across.y * offset + along.y * shift * extent.width };
                if (center.x - halfX < minimum.x + 2 * scale || center.x + halfX > maximum.x - 2 * scale ||
                    center.y - halfY < minimum.y + 2 * scale || center.y + halfY > maximum.y - 2 * scale)
                    continue;
                if (AnnotationCandidateOccupancyPenalty (occupied, center, extent, rotation, gap, measure) != 0)
                    continue;
                ScreenLabel label;
                label.anchor = { center.x, center.y - extent.height / 2 };
                label.text = input.text;
                label.rgba = input.rgba;
                label.fontSize = fontSize;
                label.centered = true;
                label.rotationRadians = rotation;
                label.resolvedPlacement = true;
                occupied.labels.push_back (label);
                return label;
            }
    return {};
}

void DrawDiagramLabel (ImDrawList& draw, const ScreenLabel& label)
{
    const auto size = ImGui::GetFont ()->CalcTextSizeA (label.fontSize, FLT_MAX, 0, label.text.c_str ());
    const ImVec2 center { label.anchor.x, label.anchor.y + size.y / 2 };
    const int first = draw.VtxBuffer.Size;
    // CPU clipping an unrotated run would drop glyphs that rotate back into the canvas.
    // Keep the draw command's canvas scissor; relax only the font's CPU clipping bounds.
    ImGui::GetFont ()->RenderText (&draw, label.fontSize, { center.x - size.x / 2, label.anchor.y }, label.rgba,
                                   { -FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX }, label.text.c_str (),
                                   label.text.c_str () + label.text.size ());
    const float cosine = std::cos (label.rotationRadians), sine = std::sin (label.rotationRadians);
    for (int i = first; i < draw.VtxBuffer.Size; ++i) {
        auto& position = draw.VtxBuffer[i].pos;
        const float x = position.x - center.x, y = position.y - center.y;
        position = { center.x + x * cosine - y * sine, center.y + x * sine + y * cosine };
    }
}
} // namespace geomsrv::archviz::hudmassingrules
