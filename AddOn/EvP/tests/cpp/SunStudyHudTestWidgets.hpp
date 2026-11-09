#ifndef TAPIOCA_TESTS_SUNSTUDYHUDTESTWIDGETS_HPP
#define TAPIOCA_TESTS_SUNSTUDYHUDTESTWIDGETS_HPP

#include <imgui.h>

#include <cmath>
#include <vector>

namespace tapioca::test {

// Find actual SmallButton backgrounds of this label's size, in draw order.
// Different labels can tie ('<' and '>'), so callers must disambiguate them.
inline std::vector<ImVec2> SunStudyTestSmallButtons (ImDrawList* draw, const char* label)
{
    std::vector<ImVec2> result;
    if (draw == nullptr || label == nullptr)
        return result;
    const auto& style = ImGui::GetStyle ();
    const float width = ImGui::CalcTextSize (label).x + 2.0f * style.FramePadding.x;
    const float height = ImGui::GetTextLineHeight (); // SmallButton deliberately has no vertical padding.
    const ImU32 colour = ImGui::GetColorU32 (ImGuiCol_Button);
    const auto near = [] (float a, float b) { return std::abs (a - b) < 0.1f; };
    for (int i = 0; i + 3 < draw->VtxBuffer.Size; ++i) {
        const auto& a = draw->VtxBuffer[i];
        const auto& b = draw->VtxBuffer[i + 1];
        const auto& c = draw->VtxBuffer[i + 2];
        const auto& d = draw->VtxBuffer[i + 3];
        if (a.col == colour && b.col == colour && c.col == colour && d.col == colour &&
            near (b.pos.x - a.pos.x, width) && near (d.pos.y - a.pos.y, height) && near (a.pos.x, d.pos.x) &&
            near (a.pos.y, b.pos.y) && near (b.pos.x, c.pos.x) && near (c.pos.y, d.pos.y))
            result.push_back (ImVec2 ((a.pos.x + c.pos.x) * 0.5f, (a.pos.y + c.pos.y) * 0.5f));
    }
    return result;
}

// DrawSunStudyTimeline renders '<' and '>' as a same-sized pair, in that order.
inline std::vector<ImVec2> SunStudyTestTimelineForwardButtons (ImDrawList* draw)
{
    const auto pair = SunStudyTestSmallButtons (draw, ">");
    std::vector<ImVec2> result;
    for (size_t i = 1; i < pair.size (); i += 2)
        result.push_back (pair[i]);
    return result;
}

} // namespace tapioca::test

#endif
