#ifndef TAPIOCA_ARCHVIZ_CAPTURELEGENDPLAN_HPP
#define TAPIOCA_ARCHVIZ_CAPTURELEGENDPLAN_HPP

#include "SunStudy/CapturePlan.hpp"
#include <imgui.h>

namespace geomsrv::archviz {
inline void DrawCaptureLegendPlan (const tapioca::sunstudy::CapturePlan* plan, float size, float north, bool northKnown)
{
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    auto* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (at, { at.x + size, at.y + size }, IM_COL32 (247, 247, 247, 255));
    if (plan) {
        const float cell = size / plan->side;
        for (int y = 0; y < plan->side; ++y)
            for (int x = 0; x < plan->side; ++x) {
                const auto v = plan->cells[y * plan->side + x];
                if (!v)
                    continue;
                const auto shade = 222 - v;
                draw->AddRectFilled ({ at.x + x * cell, at.y + y * cell },
                                     { at.x + (x + 1) * cell, at.y + (y + 1) * cell },
                                     IM_COL32 (shade, shade, shade, 255));
            }
    }
    if (northKnown) {
        const float r = north * 0.01745329252f, dx = std::cos (r), dy = -std::sin (r);
        const ImVec2 c { at.x + size * 0.25f, at.y + size * 0.26f };
        const auto ink = IM_COL32 (32, 45, 65, 255);
        draw->AddTriangleFilled (
            { c.x + dx * size * .18f, c.y + dy * size * .18f },
            { c.x - dx * size * .09f - dy * size * .06f, c.y - dy * size * .09f + dx * size * .06f },
            { c.x - dx * size * .09f + dy * size * .06f, c.y - dy * size * .09f - dx * size * .06f }, ink);
        draw->AddText ({ c.x - size * .06f, c.y + size * .17f }, ink, "N");
    }
    ImGui::Dummy ({ size, size });
}
} // namespace geomsrv::archviz
#endif
