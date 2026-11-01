// ArchViz/GradientPresetChoice -- see the header.

#include "ArchViz/GradientPresetChoice.hpp"

#include <algorithm>
#include <cstdio>

namespace geomsrv::archviz::gradientchoice {

namespace layers = overlaylayers;

namespace {

ImU32 Packed (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255);
}

std::string Range (const layers::GradientPreset& preset)
{
    char text[48] = {};
    std::snprintf (text, sizeof (text), "%g to %g", preset.defaultMin, preset.defaultMax);
    return text;
}

int IndexOf (const std::string& presetId)
{
    const auto presets = layers::GradientPresets ();
    for (size_t i = 0; i < presets.size (); ++i)
        if (presetId == presets[i].id)
            return int (i);
    return -1;
}

} // namespace

uint32_t ColourAt (std::span<const layers::ColourStop> stops, float t)
{
    if (stops.empty ())
        return 0;
    if (t <= stops.front ().at)
        return stops.front ().rgba;
    for (size_t s = 1; s < stops.size (); ++s) {
        if (t > stops[s].at)
            continue;
        const float span = stops[s].at - stops[s - 1].at;
        const float mix = span > 0.0f ? (t - stops[s - 1].at) / span : 1.0f;
        uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const float a = float ((stops[s - 1].rgba >> shift) & 255), b = float ((stops[s].rgba >> shift) & 255);
            out |= uint32_t (std::clamp (a + (b - a) * mix + 0.5f, 0.0f, 255.0f)) << shift;
        }
        return out;
    }
    return stops.back ().rgba;
}

void DrawSwatch (ImDrawList* draw, ImVec2 min, ImVec2 max, std::span<const layers::ColourStop> stops, int bands)
{
    if (stops.empty () || max.x <= min.x)
        return;
    const float w = max.x - min.x;
    if (bands > 0) {
        for (int k = 0; k < bands; ++k)
            draw->AddRectFilled (ImVec2 (min.x + w * float (k) / float (bands), min.y),
                                 ImVec2 (min.x + w * float (k + 1) / float (bands), max.y),
                                 Packed (ColourAt (stops, (float (k) + 0.5f) / float (bands))));
        return;
    }
    for (size_t s = 1; s < stops.size (); ++s) {
        const ImU32 a = Packed (stops[s - 1].rgba), b = Packed (stops[s].rgba);
        draw->AddRectFilledMultiColor (ImVec2 (min.x + w * stops[s - 1].at, min.y),
                                       ImVec2 (min.x + w * stops[s].at, max.y), a, b, b, a);
    }
}

bool GradientPresetChoice (const char* id, std::string& presetId, float width, layers::GradientCategory first,
                           const char* none)
{
    const auto presets = layers::GradientPresets ();
    ImGui::PushID (id);
    const ImGuiID original = ImGui::GetID ("##original");
    const float height = ImGui::GetFrameHeight ();
    const ImGuiStyle& style = ImGui::GetStyle ();
    const float swatch = (std::max) (24.0f, (std::min) (width * 0.35f, 7.0f * height));
    const int current = IndexOf (presetId);

    // The button: the swatch, then the name.
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const bool pressed = ImGui::InvisibleButton ("##button", ImVec2 (width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (at, ImVec2 (at.x + width, at.y + height),
                         ImGui::GetColorU32 (ImGui::IsItemHovered () ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                         style.FrameRounding);
    const float pad = style.FramePadding.x;
    if (current >= 0)
        DrawSwatch (draw, ImVec2 (at.x + pad, at.y + 4.0f), ImVec2 (at.x + pad + swatch, at.y + height - 4.0f),
                    presets[size_t (current)].Stops ());
    const char* label = current >= 0 ? presets[size_t (current)].name : none;
    draw->PushClipRect (at, ImVec2 (at.x + width - pad, at.y + height), true);
    draw->AddText (ImVec2 (at.x + 2.0f * pad + swatch, at.y + style.FramePadding.y), ImGui::GetColorU32 (ImGuiCol_Text),
                   label);
    draw->PopClipRect ();
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Colours: choose a gradient. Display only; nothing is recalculated.");
    if (pressed) {
        ImGui::GetStateStorage ()->SetInt (original, current);
        ImGui::OpenPopup ("##presets");
    }

    // The caller's window keeps what the popup opened on: inside the popup, ImGui's
    // storage is the popup's own.
    ImGuiStorage* storage = ImGui::GetStateStorage ();
    const ImGuiID open = ImGui::GetID ("##open");
    bool changed = false;
    const auto restore = [&] () {
        // -1: it opened on no preset (the caller's own colours), and goes back to none.
        const int before = storage->GetInt (original, -1);
        if (before != IndexOf (presetId)) {
            presetId = before >= 0 ? presets[size_t (before)].id : "";
            changed = true;
        }
    };
    if (!ImGui::BeginPopup ("##presets")) {
        // Escape closes the popup before this frame draws it: that is a Cancel.
        if (storage->GetBool (open) && ImGui::IsKeyPressed (ImGuiKey_Escape, false))
            restore ();
        storage->SetBool (open, false);
        ImGui::PopID ();
        return changed;
    }
    storage->SetBool (open, true);
    {
        const float rowWidth = (std::max) (16.0f * height, width);
        const float rowSwatch = 6.0f * height;
        // The list scrolls; Done and Cancel stay under it.
        ImGui::BeginChild ("##list",
                           ImVec2 (rowWidth + style.ScrollbarSize + 2.0f * style.WindowPadding.x, 16.0f * height),
                           ImGuiChildFlags_NavFlattened);
        // `first` leads, then the rest in their own order.
        const size_t lead = size_t (first);
        for (size_t pass = 0; pass < layers::kGradientCategoryCount; ++pass) {
            const auto category = layers::GradientCategory (pass == 0 ? lead : (pass <= lead ? pass - 1 : pass));
            bool headed = false;
            for (size_t i = 0; i < presets.size (); ++i) {
                const layers::GradientPreset& preset = presets[i];
                if (preset.category != category)
                    continue;
                if (!headed) {
                    ImGui::SeparatorText (layers::GradientCategoryLabel (category));
                    headed = true;
                }
                ImGui::PushID (int (i));
                const bool selected = int (i) == current;
                const ImVec2 row = ImGui::GetCursorScreenPos ();
                if (ImGui::Selectable ("##row", selected, ImGuiSelectableFlags_NoAutoClosePopups,
                                       ImVec2 (rowWidth, height)) &&
                    !selected) {
                    presetId = preset.id;
                    changed = true;
                }
                if (selected && ImGui::IsWindowAppearing ()) {
                    ImGui::SetItemDefaultFocus ();
                    ImGui::SetScrollHereY ();
                }
                ImDrawList* rows = ImGui::GetWindowDrawList ();
                DrawSwatch (rows, ImVec2 (row.x + 2.0f, row.y + 3.0f),
                            ImVec2 (row.x + rowSwatch, row.y + height - 3.0f), preset.Stops (),
                            preset.stepped ? int (preset.count) - 1 : 0);
                rows->AddText (ImVec2 (row.x + rowSwatch + pad, row.y + style.FramePadding.y),
                               ImGui::GetColorU32 (ImGuiCol_Text), preset.name);
                const std::string range = Range (preset);
                const float rangeWidth = ImGui::CalcTextSize (range.c_str ()).x;
                rows->AddText (ImVec2 (row.x + rowWidth - rangeWidth - pad, row.y + style.FramePadding.y),
                               ImGui::GetColorU32 (ImGuiCol_TextDisabled), range.c_str ());
                ImGui::PopID ();
            }
        }
        ImGui::EndChild ();
        if (ImGui::Button ("Done")) {
            storage->SetBool (open, false);
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel")) {
            restore ();
            storage->SetBool (open, false);
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }
    ImGui::PopID ();
    return changed;
}

} // namespace geomsrv::archviz::gradientchoice
