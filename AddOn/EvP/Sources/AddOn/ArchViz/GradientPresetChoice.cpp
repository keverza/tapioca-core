// ArchViz/GradientPresetChoice -- see the header.

#include "ArchViz/GradientPresetChoice.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
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

bool ColourChoice (const char* id, uint32_t& rgba, bool alpha, const char* label)
{
    ImGui::PushID (id);
    ImGuiStorage* storage = ImGui::GetStateStorage ();
    const ImGuiID original = ImGui::GetID ("##original");
    const ImGuiID open = ImGui::GetID ("##open");
    const auto vec = [] (uint32_t value) {
        return ImVec4 (float ((value >> 24) & 255) / 255.0f, float ((value >> 16) & 255) / 255.0f,
                       float ((value >> 8) & 255) / 255.0f, float (value & 255) / 255.0f);
    };
    const ImGuiColorEditFlags flags =
        ImGuiColorEditFlags_NoDragDrop |
        (alpha ? ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha);
    const bool pressed = ImGui::ColorButton ("##swatch", vec (rgba), flags);
    if (label != nullptr && label[0] != 0 && !(label[0] == '#' && label[1] == '#')) {
        ImGui::SameLine (0.0f, ImGui::GetStyle ().ItemInnerSpacing.x);
        ImGui::TextUnformatted (label, std::strstr (label, "##")); // nullptr: the whole label
    }
    if (pressed) {
        storage->SetInt (original, int (rgba));
        ImGui::OpenPopup ("##colour");
    }
    bool changed = false;
    const auto restore = [&] () {
        const uint32_t before = uint32_t (storage->GetInt (original, int (rgba)));
        changed = changed || before != rgba;
        rgba = before;
    };
    if (!ImGui::BeginPopup ("##colour")) {
        // Escape closes the popup before this frame draws it: that is a Cancel.
        if (storage->GetBool (open) && ImGui::IsKeyPressed (ImGuiKey_Escape, false))
            restore ();
        storage->SetBool (open, false);
        ImGui::PopID ();
        return changed;
    }
    storage->SetBool (open, true);
    ImVec4 colour = vec (rgba);
    const ImVec4 before = vec (uint32_t (storage->GetInt (original, int (rgba))));
    if (ImGui::ColorPicker4 ("##picker", &colour.x, flags | ImGuiColorEditFlags_NoSidePreview, &before.x)) {
        uint32_t next = 0;
        const float channels[4] = { colour.x, colour.y, colour.z, alpha ? colour.w : float (rgba & 255) / 255.0f };
        for (int c = 0; c < 4; ++c)
            next = (next << 8) | uint32_t (std::lround (std::clamp (channels[c], 0.0f, 1.0f) * 255.0f));
        changed = next != rgba;
        rgba = next;
    }
    // Before and now, side by side: the rollback is one click away.
    ImGui::ColorButton ("##before", before, flags);
    ImGui::SameLine ();
    ImGui::TextDisabled ("before");
    ImGui::SameLine ();
    ImGui::ColorButton ("##now", vec (rgba), flags);
    ImGui::SameLine ();
    ImGui::TextDisabled ("now");
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
    ImGui::PopID ();
    return changed;
}

bool EditStops (std::vector<layers::ColourStop>& stops, float width)
{
    if (stops.size () < 2)
        return false;
    ImGuiStorage* storage = ImGui::GetStateStorage ();
    const ImGuiID selectedId = ImGui::GetID ("##stops.selected");
    const ImGuiID draggingId = ImGui::GetID ("##stops.dragging");
    int selected = std::clamp (storage->GetInt (selectedId, 0), 0, int (stops.size ()) - 1);
    const float height = ImGui::GetFrameHeight ();
    const float grip = 10.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton ("##stops", ImVec2 (width, height + grip));
    const auto x = [&] (float t) { return at.x + width * t; };
    const auto tAt = [&] (float px) { return std::clamp ((px - at.x) / (std::max) (width, 1.0f), 0.0f, 1.0f); };
    bool changed = false;
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    if (ImGui::IsItemActivated ()) {
        int nearest = -1;
        float best = 6.0f;
        for (size_t i = 0; i < stops.size (); ++i)
            if (std::abs (mouse.x - x (stops[i].at)) <= best) {
                best = std::abs (mouse.x - x (stops[i].at));
                nearest = int (i);
            }
        if (nearest >= 0)
            selected = nearest;
        else if (stops.size () < layers::kMaxStops) {
            // A click on the bar between stops adds one there, in the colour already shown.
            const float t = tAt (mouse.x);
            const auto place =
                std::upper_bound (stops.begin (), stops.end (), t,
                                  [] (float value, const layers::ColourStop& stop) { return value < stop.at; });
            const layers::ColourStop added { t, ColourAt (stops, t) };
            selected = int (stops.insert (place, added) - stops.begin ());
            changed = true;
        }
        storage->SetBool (draggingId, nearest >= 0);
    }
    // The two ends stay at 0 and 1: a ramp spans its whole range.
    if (ImGui::IsItemActive () && storage->GetBool (draggingId) && selected > 0 && selected + 1 < int (stops.size ())) {
        const float lo = stops[size_t (selected) - 1].at + 0.001f, hi = stops[size_t (selected) + 1].at - 0.001f;
        const float next = std::clamp (tAt (mouse.x), lo, (std::max) (lo, hi));
        changed = changed || next != stops[size_t (selected)].at;
        stops[size_t (selected)].at = next;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    DrawSwatch (draw, at, ImVec2 (at.x + width, at.y + height), stops);
    draw->AddRect (at, ImVec2 (at.x + width, at.y + height), ImGui::GetColorU32 (ImGuiCol_Border));
    for (size_t i = 0; i < stops.size (); ++i) {
        const float px = x (stops[i].at);
        const bool on = int (i) == selected;
        draw->AddTriangleFilled (ImVec2 (px, at.y + height), ImVec2 (px - 5.0f, at.y + height + grip),
                                 ImVec2 (px + 5.0f, at.y + height + grip),
                                 on ? IM_COL32_WHITE : IM_COL32 (150, 150, 150, 255));
    }
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Click the bar to add a stop; drag a stop's marker to move it.");

    layers::ColourStop& stop = stops[size_t (selected)];
    ImGui::PushID (selected);
    changed = ColourChoice ("##stop.colour", stop.rgba, true, "stop colour") || changed;
    ImGui::PopID ();
    const bool end = selected == 0 || selected + 1 == int (stops.size ());
    ImGui::SameLine ();
    ImGui::BeginDisabled (end);
    float position = stop.at * 100.0f;
    ImGui::SetNextItemWidth ((std::max) (60.0f, width * 0.3f));
    if (ImGui::SliderFloat ("##stop.at", &position, 0.0f, 100.0f, "%.0f %%") && !end) {
        const float lo = stops[size_t (selected) - 1].at + 0.001f, hi = stops[size_t (selected) + 1].at - 0.001f;
        stop.at = std::clamp (position / 100.0f, lo, (std::max) (lo, hi));
        changed = true;
    }
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    ImGui::BeginDisabled (end || stops.size () <= 2);
    if (ImGui::SmallButton ("Remove stop")) {
        stops.erase (stops.begin () + selected);
        selected = (std::max) (0, selected - 1);
        changed = true;
    }
    ImGui::EndDisabled ();
    storage->SetInt (selectedId, selected);
    return changed;
}

namespace {

// What a picker's popup opened on, for its Cancel: the preset and the custom stops.
struct Opened {
    std::string preset;
    std::vector<layers::ColourStop> custom;
};
std::map<ImGuiID, Opened> g_opened; // the ImGui thread's

bool SameStops (const std::vector<layers::ColourStop>& a, const std::vector<layers::ColourStop>& b)
{
    return a.size () == b.size () &&
           std::equal (a.begin (), a.end (), b.begin (), [] (const layers::ColourStop& x, const layers::ColourStop& y) {
               return x.at == y.at && x.rgba == y.rgba;
           });
}

std::span<const layers::ColourStop> StopsOf (const std::string& presetId, const std::vector<layers::ColourStop>& custom)
{
    if (presetId == layers::kCustomGradientId)
        return custom;
    const layers::GradientPreset* preset = layers::FindGradientPreset (presetId);
    return preset != nullptr ? preset->Stops () : std::span<const layers::ColourStop> ();
}

} // namespace

bool GradientPresetChoice (const char* id, std::string& presetId, std::vector<layers::ColourStop>& custom, float width,
                           layers::GradientCategory first, const char* none)
{
    const auto presets = layers::GradientPresets ();
    ImGui::PushID (id);
    const ImGuiID original = ImGui::GetID ("##original");
    const float height = ImGui::GetFrameHeight ();
    const ImGuiStyle& style = ImGui::GetStyle ();
    const float swatch = (std::max) (24.0f, (std::min) (width * 0.35f, 7.0f * height));
    const bool isCustom = presetId == layers::kCustomGradientId;
    const int current = IndexOf (presetId);

    // The button: the swatch, then the name.
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const bool pressed = ImGui::InvisibleButton ("##button", ImVec2 (width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (at, ImVec2 (at.x + width, at.y + height),
                         ImGui::GetColorU32 (ImGui::IsItemHovered () ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                         style.FrameRounding);
    const float pad = style.FramePadding.x;
    const auto shown = StopsOf (presetId, custom);
    if (shown.size () >= 2)
        DrawSwatch (draw, ImVec2 (at.x + pad, at.y + 4.0f), ImVec2 (at.x + pad + swatch, at.y + height - 4.0f), shown);
    const char* label = current >= 0 ? presets[size_t (current)].name : isCustom || none == nullptr ? "Custom" : none;
    draw->PushClipRect (at, ImVec2 (at.x + width - pad, at.y + height), true);
    draw->AddText (ImVec2 (at.x + 2.0f * pad + swatch, at.y + style.FramePadding.y), ImGui::GetColorU32 (ImGuiCol_Text),
                   label);
    draw->PopClipRect ();
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Colours: choose a gradient or make your own. Display only; nothing is recalculated.");
    if (pressed) {
        g_opened[original] = { presetId, custom };
        ImGui::OpenPopup ("##presets");
    }

    // The caller's window keeps the open flag: inside the popup, ImGui's storage is
    // the popup's own.
    ImGuiStorage* storage = ImGui::GetStateStorage ();
    const ImGuiID open = ImGui::GetID ("##open");
    bool changed = false;
    const auto restore = [&] () {
        const auto before = g_opened.find (original);
        if (before == g_opened.end ())
            return;
        if (before->second.preset != presetId || !SameStops (before->second.custom, custom)) {
            presetId = before->second.preset;
            custom = before->second.custom;
            changed = true;
        }
    };
    const auto close = [&] () {
        storage->SetBool (open, false);
        g_opened.erase (original);
    };
    if (!ImGui::BeginPopup ("##presets")) {
        // Escape closes the popup before this frame draws it: that is a Cancel.
        if (storage->GetBool (open)) {
            if (ImGui::IsKeyPressed (ImGuiKey_Escape, false))
                restore ();
            close ();
        }
        ImGui::PopID ();
        return changed;
    }
    storage->SetBool (open, true);
    const float rowWidth = (std::max) (16.0f * height, width);
    const float rowSwatch = 6.0f * height;
    const auto row = [&] (const char* name, std::span<const layers::ColourStop> stops, bool selected, int bands,
                          const char* note) {
        const ImVec2 corner = ImGui::GetCursorScreenPos ();
        const bool picked =
            ImGui::Selectable ("##row", selected, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2 (rowWidth, height)) &&
            !selected;
        if (selected && ImGui::IsWindowAppearing ()) {
            ImGui::SetItemDefaultFocus ();
            ImGui::SetScrollHereY ();
        }
        ImDrawList* rows = ImGui::GetWindowDrawList ();
        if (stops.size () >= 2)
            DrawSwatch (rows, ImVec2 (corner.x + 2.0f, corner.y + 3.0f),
                        ImVec2 (corner.x + rowSwatch, corner.y + height - 3.0f), stops, bands);
        rows->AddText (ImVec2 (corner.x + rowSwatch + pad, corner.y + style.FramePadding.y),
                       ImGui::GetColorU32 (ImGuiCol_Text), name);
        if (note != nullptr) {
            const float noteWidth = ImGui::CalcTextSize (note).x;
            rows->AddText (ImVec2 (corner.x + rowWidth - noteWidth - pad, corner.y + style.FramePadding.y),
                           ImGui::GetColorU32 (ImGuiCol_TextDisabled), note);
        }
        return picked;
    };
    // The list scrolls; the stop editor, Done and Cancel stay under it.
    ImGui::BeginChild ("##list", ImVec2 (rowWidth + style.ScrollbarSize + 2.0f * style.WindowPadding.x, 14.0f * height),
                       ImGuiChildFlags_NavFlattened);
    ImGui::SeparatorText ("Yours");
    if (none != nullptr) {
        ImGui::PushID ("##none");
        if (row (none, {}, presetId.empty (), 0, "the colours set beside")) {
            presetId.clear ();
            changed = true;
        }
        ImGui::PopID ();
    }
    ImGui::PushID ("##custom");
    if (row ("Custom...", custom, isCustom, 0, "edit the stops")) {
        // Starts from what is shown, so a tweak of a preset is one click away.
        if (shown.size () >= 2)
            custom.assign (shown.begin (), shown.end ());
        if (custom.size () < 2)
            custom = { { 0.0f, 0x000000FFu }, { 1.0f, 0xFFFFFFFFu } };
        presetId = layers::kCustomGradientId;
        changed = true;
    }
    ImGui::PopID ();
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
            const std::string range = Range (preset);
            if (row (preset.name, preset.Stops (), int (i) == current, preset.stepped ? int (preset.count) - 1 : 0,
                     range.c_str ())) {
                presetId = preset.id;
                changed = true;
            }
            ImGui::PopID ();
        }
    }
    ImGui::EndChild ();
    if (presetId == layers::kCustomGradientId) {
        ImGui::SeparatorText ("Custom stops");
        changed = EditStops (custom, rowWidth) || changed;
    }
    ImGui::Separator ();
    if (ImGui::Button ("Done")) {
        close ();
        ImGui::CloseCurrentPopup ();
    }
    ImGui::SameLine ();
    if (ImGui::Button ("Cancel")) {
        restore ();
        close ();
        ImGui::CloseCurrentPopup ();
    }
    ImGui::SameLine ();
    ImGui::TextDisabled ("Esc cancels");
    ImGui::EndPopup ();
    ImGui::PopID ();
    return changed;
}

} // namespace geomsrv::archviz::gradientchoice
