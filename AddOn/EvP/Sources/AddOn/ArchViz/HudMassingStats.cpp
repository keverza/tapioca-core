#include "ArchViz/HudMassingStats.hpp"
#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace geomsrv::archviz::hudmassingstats {
namespace {
ImU32 Colour (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
}
} // namespace
std::string Draw (const massingslices::Result& result, const overlaylayers::Panel& look, float scale)
{
    std::string hovered;
    if (result.rows.empty ())
        return hovered;
    ImGui::PushID ("massing.stats.mix");
    ImGui::SeparatorText ("Floor-area mix");
    ImGui::TextDisabled (result.clipped ? "Allowed area by function" : "Slab area by function (envelope pending)");
    const auto mix = massingslices::UsageMix (result);
    const float width = (std::max) (1.0f, ImGui::GetContentRegionAvail ().x);
    const float height = ImGui::GetFrameHeight ();
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    auto* draw = ImGui::GetWindowDrawList ();
    float offset = 0;
    for (const auto& use : mix) {
        const float span = width * float (use.percent / 100);
        const ImVec2 lo { start.x + offset, start.y }, hi { start.x + offset + span, start.y + height };
        draw->AddRectFilled (lo, hi, Colour (use.rgba));
        char caption[32];
        std::snprintf (caption, sizeof (caption), "%.2f%%", use.percent);
        const auto textSize = ImGui::CalcTextSize (caption);
        if (textSize.x + 8 * scale <= span)
            draw->AddText ({ lo.x + (span - textSize.x) / 2, lo.y + (height - textSize.y) / 2 },
                           hudshell::Packed (hudshell::Contrast (use.rgba)), caption);
        offset += span;
    }
    ImGui::InvisibleButton ("##mix", { width, height });
    if (ImGui::IsItemHovered ()) {
        const float at = (ImGui::GetIO ().MousePos.x - start.x) / width * 100;
        double end = 0;
        for (const auto& use : mix) {
            end += use.percent;
            if (at < end) {
                hovered = use.function;
                break;
            }
        }
    }
    for (const auto& use : mix) {
        ImGui::PushID (use.function.c_str ());
        char caption[96];
        std::snprintf (caption, sizeof (caption), "%.2f%% (%.2f m2)", use.percent, use.area);
        hudshell::Card legend;
        legend.figures.push_back ({ use.label, caption, use.rgba });
        const auto rowStart = ImGui::GetCursorScreenPos ();
        hudshell::Cards ({ legend }, look, scale);
        if (ImGui::IsWindowHovered () &&
            ImGui::IsMouseHoveringRect (
                rowStart, { rowStart.x + width, ImGui::GetCursorScreenPos ().y - ImGui::GetStyle ().ItemSpacing.y }))
            hovered = use.function;
        ImGui::PopID ();
    }
    if (!hovered.empty ()) {
        ImGui::BeginTooltip ();
        for (const auto& use : mix)
            if (use.function == hovered) {
                ImGui::Text ("%s: %.2f%%", use.label.c_str (), use.percent);
                ImGui::Text ("Area: %.2f m2", use.area);
                ImGui::Text ("Slice-estimated volume: %.2f m3", use.volume);
            }
        ImGui::TextDisabled ("Highlight extends each slice to the next floor / slab top.");
        ImGui::EndTooltip ();
    }
    if (mix.empty ())
        ImGui::TextDisabled ("No floor area to distribute.");
    if (result.hasCoverage && result.parcelArea > 0) {
        ImGui::SeparatorText ("Parcel coverage");
        const float coverage = float (result.builtArea / result.parcelArea);
        char caption[160];
        std::snprintf (caption, sizeof (caption), "Built %.2f%% / Unbuilt %.2f%%", 100 * coverage,
                       100 * (1 - coverage));
        ImGui::ProgressBar (coverage, { -1, 0 }, caption);
        ImGui::TextDisabled ("Projected slab union within each parcel; not summed floor areas.");
    }
    ImGui::PopID ();
    return hovered;
}
} // namespace geomsrv::archviz::hudmassingstats
