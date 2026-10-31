#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/HudShell.hpp"
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

void DrawSunStudyRampBar (ImDrawList* draw, ImVec2 start, ImVec2 size, int bands, const float* edges, float blueBelow)
{
    const float top = kSunRampTop;
    const bool stepped = edges != nullptr && bands >= kSunBandsMin && bands <= kSunBandsMax;
    const auto position = [&] (float hours) { return start.x + size.x * hours / top; };
    if (stepped) {
        for (int band = 0; band < bands; ++band) {
            const float lo = band > 0 ? edges[band - 1] : 0.0f;
            const float hi = band < bands - 1 ? edges[band] : top;
            draw->AddRectFilled (ImVec2 (position (lo), start.y), ImVec2 (position (hi), start.y + size.y),
                                 SunColour (SunStudyBandCentre (band, bands, edges)));
        }
        for (int i = 0; i < bands - 1; ++i)
            draw->AddLine (ImVec2 (position (edges[i]), start.y), ImVec2 (position (edges[i]), start.y + size.y),
                           IM_COL32 (40, 40, 40, 200), 1.0f);
    }
    else {
        for (int segment = 0; segment < 40; ++segment) {
            const ImVec2 lo (start.x + size.x * segment / 40.0f, start.y);
            const ImVec2 hi (start.x + size.x * (segment + 1) / 40.0f, start.y + size.y);
            const ImU32 a = SunColour (float (segment) * 0.25f), b = SunColour (float (segment + 1) * 0.25f);
            draw->AddRectFilledMultiColor (lo, hi, a, b, b, a);
        }
    }
    if (blueBelow >= 0.0f)
        draw->AddRectFilled (start, ImVec2 (position ((std::min) (blueBelow, top)), start.y + size.y),
                             IM_COL32 (40, 83, 107, 153));
}

void DrawSunStudyGradient (bool blue, float& threshold, int bands, float* edges)
{
    constexpr float top = kSunRampTop;
    const bool stepped = edges != nullptr && bands >= kSunBandsMin && bands <= kSunBandsMax;
    threshold = std::clamp (threshold, 0.0f, top);
    // The grips stand ABOVE the bar, the blue box below it, so the two never
    // sit on the same pixels.
    const float gripRoom = stepped ? 10.0f : 0.0f;
    const ImVec2 widget = ImGui::GetCursorScreenPos ();
    const ImVec2 start (widget.x, widget.y + gripRoom);
    const float width = ImGui::GetContentRegionAvail ().x;
    const float height = ImGui::GetFrameHeight ();
    const auto position = [&] (float hours) { return start.x + width * hours / top; };
    const auto hoursAt = [&] (float x) {
        return std::clamp (Snapped ((x - start.x) / std::max (width, 1.0f) * top), 0.0f, top);
    };
    ImGui::InvisibleButton ("##sun-gradient", ImVec2 (width, gripRoom + height + (blue ? 12.0f : 0.0f)));
    // Which handle the press took: -1 the blue box, 0.. an edge, -2 nothing.
    const ImGuiID handleId = ImGui::GetID ("##sun-gradient-handle");
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    if (ImGui::IsItemActivated ()) {
        int handle = blue ? -1 : -2;
        if (stepped && (mouse.y < start.y + height || !blue)) {
            float best = 1.0e9f;
            for (int i = 0; i < bands - 1; ++i)
                if (std::abs (mouse.x - position (edges[i])) < best) {
                    best = std::abs (mouse.x - position (edges[i]));
                    handle = i;
                }
        }
        ImGui::GetStateStorage ()->SetInt (handleId, handle);
    }
    if (ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
        const int handle = ImGui::GetStateStorage ()->GetInt (handleId, -2);
        if (handle == -1)
            threshold = hoursAt (mouse.x);
        else if (stepped && handle >= 0 && handle < bands - 1) {
            // Held between its neighbours, never crossing them.
            const float lo = handle > 0 ? edges[handle - 1] + kSunBandGap : kSunBandGap;
            const float hi = handle < bands - 2 ? edges[handle + 1] - kSunBandGap : top - kSunBandGap;
            edges[handle] = std::clamp (hoursAt (mouse.x), lo, hi);
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    DrawSunStudyRampBar (draw, start, ImVec2 (width, height), stepped ? bands : 0, edges, blue ? threshold : -1.0f);
    if (stepped)
        for (int i = 0; i < bands - 1; ++i) {
            const float x = position (edges[i]);
            draw->AddTriangleFilled (ImVec2 (x - 5, widget.y + 1), ImVec2 (x + 5, widget.y + 1),
                                     ImVec2 (x, start.y - 1), IM_COL32 (226, 226, 226, 255));
        }
    if (blue) {
        const float edge = position (threshold);
        draw->AddRectFilled (ImVec2 (edge - 5, start.y + height - 3), ImVec2 (edge + 5, start.y + height + 9),
                             Colour (0x28536B), 2.0f);
        draw->AddRect (ImVec2 (edge - 5, start.y + height - 3), ImVec2 (edge + 5, start.y + height + 9), IM_COL32_WHITE,
                       2.0f);
    }
    if (ImGui::IsItemHovered ()) {
        std::string tip;
        if (stepped)
            tip = "Drag a grip above the bar to move a step's edge";
        if (blue)
            tip += std::string (tip.empty () ? "" : "; ") + "drag the blue box: direct sun below " +
                   SunStudyClock (threshold, true);
        if (!tip.empty ())
            hudshell::Tip (tip, hudshell::TipSide::Above);
    }
    if (!stepped) {
        ImGui::TextDisabled ("0:00h                            9:00h+");
        return;
    }
    // The bands' hour ranges, the legend's words for the swatches above.
    std::string ranges;
    for (int band = 0; band < bands; ++band) {
        const float lo = band > 0 ? edges[band - 1] : 0.0f;
        ranges +=
            (band > 0 ? "  " : "") + SunStudyClock (lo) + (band < bands - 1 ? "-" + SunStudyClock (edges[band]) : "+");
    }
    ImGui::PushStyleColor (ImGuiCol_Text, ImGui::GetStyleColorVec4 (ImGuiCol_TextDisabled));
    ImGui::TextWrapped ("%s h", ranges.c_str ());
    ImGui::PopStyleColor ();
}

void DrawSunStudyInspectControl (int& mode, const char* first, const char* second)
{
    ImGui::TextUnformatted ("Mouse hover");
    const char* labels[] = { "Off##suninspect", "Cursor##suninspect", "Panel##suninspect" };
    const float width = (ImGui::GetContentRegionAvail ().x - 2.0f * ImGui::GetStyle ().ItemSpacing.x) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        if (i != 0)
            ImGui::SameLine ();
        ImGui::PushStyleColor (ImGuiCol_Button,
                               ImGui::GetStyleColorVec4 (mode == i ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        if (ImGui::Button (labels[i], ImVec2 (width, 0)))
            mode = i;
        ImGui::PopStyleColor ();
    }
    if (mode != 2)
        return;
    // Fixed viewport prevents both height and width from following hover text.
    ImGui::BeginChild ("##sun-reading", ImVec2 (0, 3.0f * ImGui::GetTextLineHeightWithSpacing ()), ImGuiChildFlags_None,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextWrapped ("%s", first != nullptr && first[0] != 0 ? first : "hover the model");
    if (second != nullptr && second[0] != 0) {
        ImGui::PushStyleColor (ImGuiCol_Text, ImGui::GetStyleColorVec4 (ImGuiCol_TextDisabled));
        ImGui::TextWrapped ("%s", second);
        ImGui::PopStyleColor ();
    }
    ImGui::EndChild ();
}
} // namespace geomsrv::archviz
