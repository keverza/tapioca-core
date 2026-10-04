#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/SunStudyPreview.hpp"

#include <imgui.h>

namespace geomsrv::archviz {
namespace {
constexpr unsigned kColours[10] = { 0x6b3d18, 0x8a4f1f, 0x9c5a23, 0xb06a28, 0xc07d33,
                                    0xcf8f44, 0xdca157, 0xe7b674, 0xf0cb96, 0xf7e3c2 };
ImU32 Colour (unsigned hex)
{
    return IM_COL32 ((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, 255);
}
ImU32 SunColour (float hours)
{
    // The same half-hour-centred warm ramp as SunRamp in the tint shader.
    const float t = std::clamp (hours - 0.5f, 0.0f, 9.0f);
    const int lo = int (std::floor (t)), hi = std::min (lo + 1, 9);
    const float mix = t - float (lo);
    const auto channel = [&] (int shift) {
        const float a = float ((kColours[lo] >> shift) & 255), b = float ((kColours[hi] >> shift) & 255);
        return int (std::round (a + (b - a) * mix));
    };
    return IM_COL32 (channel (16), channel (8), channel (0), 255);
}
float Snapped (float value)
{
    return std::round (value * 4.0f) / 4.0f;
}
} // namespace

bool DrawSunStudyRange (const char* id, float& from, float& to, float minimum, float maximum, bool duration)
{
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    const float height = ImGui::GetFrameHeight () + 4.0f;
    const float width = ImGui::GetContentRegionAvail ().x;
    const float inset = 7.0f;
    const float span = std::max (maximum - minimum, 0.25f);
    from = std::clamp (from, minimum, maximum);
    to = std::clamp (to, from, maximum);
    const auto position = [&] (float value) {
        return start.x + inset + (width - 2 * inset) * (value - minimum) / span;
    };
    ImGui::InvisibleButton (id, ImVec2 (width, height));
    const ImGuiID handleId = ImGui::GetID (id);
    if (ImGui::IsItemActivated ()) {
        const float mouse = ImGui::GetIO ().MousePos.x;
        ImGui::GetStateStorage ()->SetInt (
            handleId, std::abs (mouse - position (from)) <= std::abs (mouse - position (to)) ? 0 : 1);
    }
    bool changed = false;
    if (ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
        const float value = std::clamp (Snapped (minimum + (ImGui::GetIO ().MousePos.x - start.x - inset) /
                                                               std::max (width - 2 * inset, 1.0f) * span),
                                        minimum, maximum);
        float& chosen = ImGui::GetStateStorage ()->GetInt (handleId) == 0 ? from : to;
        const float next = &chosen == &from ? std::min (value, to) : std::max (value, from);
        changed = chosen != next;
        chosen = next;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (start, ImVec2 (start.x + width, start.y + height), IM_COL32 (76, 76, 76, 255), 4.0f);
    const float left = position (from), right = position (to);
    draw->AddRectFilled (ImVec2 (left, start.y), ImVec2 (right, start.y + height), IM_COL32 (109, 102, 90, 255), 4.0f);
    for (const float x : { left, right })
        draw->AddRectFilled (ImVec2 (x - 3, start.y + 3), ImVec2 (x + 3, start.y + height - 3),
                             IM_COL32 (226, 226, 226, 255), 3.0f);
    const std::string text = SunStudyClock (from, duration) + " to " + SunStudyClock (to, duration);
    const ImVec2 size = ImGui::CalcTextSize (text.c_str ());
    draw->AddText (ImVec2 (start.x + (width - size.x) * 0.5f, start.y + (height - size.y) * 0.5f),
                   IM_COL32 (255, 216, 158, 255), text.c_str ());
    return changed;
}

void DrawSunStudyGradient (bool blue, float& threshold)
{
    constexpr float top = 10.0f;
    threshold = std::clamp (threshold, 0.0f, top);
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    const float width = ImGui::GetContentRegionAvail ().x;
    const float height = ImGui::GetFrameHeight ();
    ImGui::InvisibleButton ("##sun-gradient", ImVec2 (width, height + (blue ? 12.0f : 0.0f)));
    if (blue && ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left))
        threshold =
            std::clamp (Snapped ((ImGui::GetIO ().MousePos.x - start.x) / std::max (width, 1.0f) * top), 0.0f, top);
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    for (int segment = 0; segment < 40; ++segment) {
        const ImVec2 lo (start.x + width * segment / 40.0f, start.y);
        const ImVec2 hi (start.x + width * (segment + 1) / 40.0f, start.y + height);
        const ImU32 a = SunColour (float (segment) * 0.25f), b = SunColour (float (segment + 1) * 0.25f);
        draw->AddRectFilledMultiColor (lo, hi, a, b, b, a);
    }
    if (blue) {
        const float edge = start.x + width * threshold / top;
        draw->AddRectFilled (start, ImVec2 (edge, start.y + height), Colour (0x28536B));
        draw->AddRectFilled (ImVec2 (edge - 5, start.y + height - 3), ImVec2 (edge + 5, start.y + height + 9),
                             Colour (0x28536B), 2.0f);
        draw->AddRect (ImVec2 (edge - 5, start.y + height - 3), ImVec2 (edge + 5, start.y + height + 9), IM_COL32_WHITE,
                       2.0f);
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Drag the blue box: direct sun below %s", SunStudyClock (threshold, true).c_str ());
    }
    ImGui::TextDisabled ("0:00h                            9:00h+");
}
} // namespace geomsrv::archviz
