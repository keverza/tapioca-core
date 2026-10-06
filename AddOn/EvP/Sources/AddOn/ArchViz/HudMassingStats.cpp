#include "ArchViz/HudMassingStats.hpp"
#include "ArchViz/GraphicsSettings.hpp"
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
bool CoefficientInputs (massingareas::Coefficients& coefficients, std::vector<massingareas::NumberEdit>& numbers)
{
    if (!ImGui::CollapsingHeader ("Area calculation coefficients", ImGuiTreeNodeFlags_DefaultOpen))
        return false;
    auto wanted = coefficients;
    bool edited = false;
    if (ImGui::BeginTable ("##massing.coefficients", 2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn ("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("##value", ImGuiTableColumnFlags_WidthFixed, 9 * ImGui::GetFontSize ());
        const auto input = [&] (const char* label, double& value, double low, double high) {
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted (label);
            ImGui::TableSetColumnIndex (1);
            ImGui::PushID (label);
            ImGui::SetNextItemWidth (ImGui::GetContentRegionAvail ().x - ImGui::CalcTextSize ("Set").x -
                                     2 * ImGui::GetStyle ().FramePadding.x - ImGui::GetStyle ().ItemSpacing.x);
            edited |= ImGui::DragScalar ("##value", ImGuiDataType_Double, &value, high == 1 ? 0.005f : 0.5f, &low,
                                         &high, "%.3f", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine ();
            if (ImGui::SmallButton ("Set"))
                numbers.push_back ({ wanted, label, value, low, high });
            ImGui::PopID ();
        };
        input ("Gross area factor", wanted.grossFactor, 0, 1);
        input ("Sellable area factor", wanted.sellableFactor, 0, 1);
        input ("Gross m2 per unit", wanted.unitGrossArea, 0.01, 1000000);
        input ("Parking m2 per unit", wanted.parkingAreaPerUnit, 0, 1000000);
        ImGui::EndTable ();
    }
    ImGui::TextDisabled ("Gross = total x factor; sellable = total x factor.");
    ImGui::TextDisabled ("Units = gross / m2 per unit; parking = units x m2 per unit.");
    if (!edited)
        return false;
    if (!massingareas::Valid (wanted)) {
        ImGui::TextDisabled ("Factors: 0..1; unit area: 0.01..1000000; parking: 0..1000000.");
        return false;
    }
    coefficients = wanted;
    return true;
}

std::string Draw (const massingslices::Result& result, const overlaylayers::Panel& look, float scale)
{
    std::string hovered;
    if (result.rows.empty () && !result.hasCoverage)
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
        const auto colour = graphicssettings::FunctionColour (use.function, use.rgba);
        draw->AddRectFilled (lo, hi, Colour (colour));
        char caption[32];
        std::snprintf (caption, sizeof (caption), "%.2f%%", use.percent);
        const auto textSize = ImGui::CalcTextSize (caption);
        if (textSize.x + 8 * scale <= span)
            draw->AddText ({ lo.x + (span - textSize.x) / 2, lo.y + (height - textSize.y) / 2 },
                           hudshell::Packed (hudshell::Contrast (colour)), caption);
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
        const float coverage = float (std::clamp (result.builtArea / result.parcelArea, 0.0, 1.0));
        char caption[160];
        std::snprintf (caption, sizeof (caption), "Built %.2f%% / Unbuilt %.2f%%", 100 * coverage,
                       100 * (1 - coverage));
        ImGui::PushStyleColor (ImGuiCol_PlotHistogram,
                               hudshell::Colour (graphicssettings::Colour ("ui.coverage.built", 0x9AA0A6FF)));
        ImGui::PushStyleColor (ImGuiCol_FrameBg,
                               hudshell::Colour (graphicssettings::Colour ("ui.coverage.unbuilt", 0x66BB6AFF)));
        ImGui::ProgressBar (coverage, { -1, 0 }, caption);
        if (ImGui::IsItemHovered ()) {
            const float at = (ImGui::GetIO ().MousePos.x - ImGui::GetItemRectMin ().x) /
                             (ImGui::GetItemRectMax ().x - ImGui::GetItemRectMin ().x);
            const bool built = at < coverage;
            hovered = built ? massingslices::kBuiltHover : massingslices::kUnbuiltHover;
            ImGui::SetTooltip ("%s: %.2f m2 (%.2f%%)", built ? "Built footprint" : "Unbuilt parcel",
                               built ? result.builtArea : result.unbuiltArea, 100 * (built ? coverage : 1 - coverage));
        }
        ImGui::PopStyleColor (2);
        ImGui::TextDisabled ("Projected slab union within each parcel; not summed floor areas.");
    }
    ImGui::PopID ();
    return hovered;
}
} // namespace geomsrv::archviz::hudmassingstats
