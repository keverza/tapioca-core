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

void OpenColourPopup (const char* id, uint32_t rgba)
{
    ImGui::PushID (id);
    ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("##original"), int (rgba));
    ImGui::OpenPopup ("##colour");
    ImGui::PopID ();
}

bool ColourPopup (const char* id, uint32_t& rgba, bool alpha)
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

bool ColourChoice (const char* id, uint32_t& rgba, bool alpha, const char* label)
{
    ImGui::PushID (id);
    const ImVec4 colour (float ((rgba >> 24) & 255) / 255.0f, float ((rgba >> 16) & 255) / 255.0f,
                         float ((rgba >> 8) & 255) / 255.0f, float (rgba & 255) / 255.0f);
    const bool pressed = ImGui::ColorButton (
        "##swatch", colour,
        ImGuiColorEditFlags_NoDragDrop | (alpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha));
    if (label != nullptr && label[0] != 0 && !(label[0] == '#' && label[1] == '#')) {
        ImGui::SameLine (0.0f, ImGui::GetStyle ().ItemInnerSpacing.x);
        ImGui::TextUnformatted (label, std::strstr (label, "##")); // nullptr: the whole label
    }
    ImGui::PopID ();
    if (pressed)
        OpenColourPopup (id, rgba);
    return ColourPopup (id, rgba, alpha);
}

// ---- a custom ramp's stops ------------------------------------------------------

namespace {

int PreviousColour (const std::vector<layers::ColourStop>& stops, int i)
{
    for (int k = i - 1; k >= 0; --k)
        if (!stops[size_t (k)].hint)
            return k;
    return -1;
}

int NextColour (const std::vector<layers::ColourStop>& stops, int i)
{
    for (int k = i + 1; k < int (stops.size ()); ++k)
        if (!stops[size_t (k)].hint)
            return k;
    return -1;
}

uint32_t Mix (uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= ((((a >> shift) & 255u) + ((b >> shift) & 255u) + 1u) / 2u) << shift;
    return out;
}

// The hint stop between colour stops `a` and `b`, or -1.
int HintBetween (const std::vector<layers::ColourStop>& stops, int a, int b)
{
    for (int k = a + 1; k < b; ++k)
        if (stops[size_t (k)].hint)
            return k;
    return -1;
}

} // namespace

int ColourStopCount (const std::vector<layers::ColourStop>& stops)
{
    return int (std::count_if (stops.begin (), stops.end (), [] (const auto& stop) { return !stop.hint; }));
}

void RefreshHints (std::vector<layers::ColourStop>& stops)
{
    for (int i = 0; i < int (stops.size ()); ++i) {
        if (!stops[size_t (i)].hint)
            continue;
        const int a = PreviousColour (stops, i), b = NextColour (stops, i);
        if (a < 0 || b < 0) {
            stops.erase (stops.begin () + i--); // a hint outside two colours means nothing
            continue;
        }
        stops[size_t (i)].rgba = Mix (stops[size_t (a)].rgba, stops[size_t (b)].rgba);
    }
}

int AddColourStop (std::vector<layers::ColourStop>& stops, float t)
{
    if (stops.size () < 2)
        return -1;
    t = std::clamp (t, 0.0f, 1.0f);
    int a = -1, b = -1;
    for (int i = 0; i < int (stops.size ()); ++i)
        if (!stops[size_t (i)].hint) {
            if (stops[size_t (i)].at <= t)
                a = i;
            else if (b < 0)
                b = i;
        }
    if (a < 0 || b < 0 || t <= stops[size_t (a)].at + 0.005f || t >= stops[size_t (b)].at - 0.005f)
        return -1;
    const uint32_t colour = ColourAt (stops, t); // the colour already shown there
    // The segment is split: its blend midpoint no longer means anything.
    if (const int hint = HintBetween (stops, a, b); hint >= 0)
        stops.erase (stops.begin () + hint);
    if (stops.size () >= layers::kMaxStops)
        return -1;
    const auto place = std::upper_bound (stops.begin (), stops.end (), t,
                                         [] (float value, const auto& stop) { return value < stop.at; });
    const int index = int (place - stops.begin ());
    stops.insert (place, layers::ColourStop { t, colour, false });
    RefreshHints (stops);
    return index;
}

bool RemoveColourStop (std::vector<layers::ColourStop>& stops, int index)
{
    if (index < 0 || index >= int (stops.size ()) || stops[size_t (index)].hint || ColourStopCount (stops) <= 2)
        return false;
    const int a = PreviousColour (stops, index), b = NextColour (stops, index);
    // Its two blends go with it, from the far end first so the indices hold.
    if (b >= 0)
        if (const int hint = HintBetween (stops, index, b); hint >= 0)
            stops.erase (stops.begin () + hint);
    stops.erase (stops.begin () + index);
    if (a >= 0)
        if (const int hint = HintBetween (stops, a, index); hint >= 0)
            stops.erase (stops.begin () + hint);
    // An end removed: the colour next to it becomes the end, so the ramp still spans 0..1.
    stops.front ().at = 0.0f;
    stops.back ().at = 1.0f;
    RefreshHints (stops);
    return true;
}

bool MoveColourStop (std::vector<layers::ColourStop>& stops, int index, float t)
{
    const int a = PreviousColour (stops, index), b = NextColour (stops, index);
    if (index < 0 || index >= int (stops.size ()) || stops[size_t (index)].hint || a < 0 || b < 0)
        return false; // the ends stay at 0 and 1
    const float lo = stops[size_t (a)].at, hi = stops[size_t (b)].at;
    t = std::clamp (t, lo + 0.01f, (std::max) (lo + 0.01f, hi - 0.01f));
    const float old = stops[size_t (index)].at;
    if (t == old)
        return false;
    // The blends either side keep their place within their segment.
    const int left = HintBetween (stops, a, index), right = HintBetween (stops, index, b);
    if (left >= 0)
        stops[size_t (left)].at = lo + (stops[size_t (left)].at - lo) / (old - lo) * (t - lo);
    if (right >= 0)
        stops[size_t (right)].at = t + (stops[size_t (right)].at - old) / (hi - old) * (hi - t);
    stops[size_t (index)].at = t;
    return true;
}

bool SetBlendMidpoint (std::vector<layers::ColourStop>& stops, int colour, float t)
{
    const int b = NextColour (stops, colour);
    if (colour < 0 || colour >= int (stops.size ()) || stops[size_t (colour)].hint || b < 0)
        return false;
    const float lo = stops[size_t (colour)].at, hi = stops[size_t (b)].at;
    t = std::clamp (t, lo + 0.005f, (std::max) (lo + 0.005f, hi - 0.005f));
    const bool centred = std::abs (t - 0.5f * (lo + hi)) < 0.02f * (hi - lo);
    const int hint = HintBetween (stops, colour, b);
    if (hint >= 0) {
        if (centred) // back at the middle: an even blend needs no hint
            stops.erase (stops.begin () + hint);
        else
            stops[size_t (hint)].at = t;
        RefreshHints (stops);
        return true;
    }
    if (centred || stops.size () >= layers::kMaxStops)
        return false;
    stops.insert (stops.begin () + b, layers::ColourStop { t, 0u, true });
    RefreshHints (stops);
    return true;
}

bool EditStops (std::vector<layers::ColourStop>& stops, float width)
{
    if (stops.size () < 2)
        return false;
    ImGuiStorage* storage = ImGui::GetStateStorage ();
    const ImGuiID selectedId = ImGui::GetID ("##stops.selected");
    const ImGuiID kindId = ImGui::GetID ("##stops.kind");
    const ImGuiID indexId = ImGui::GetID ("##stops.index");
    const ImGuiID movedId = ImGui::GetID ("##stops.moved");
    enum Kind { None, Colour, Blend };
    const float bar = 1.4f * ImGui::GetFrameHeight ();
    const float radius = 0.4f * bar;
    const float arrows = 12.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const float left = at.x + radius, span = (std::max) (1.0f, width - 2.0f * radius); // circles stay inside
    const float middle = at.y + 0.5f * bar;
    const auto x = [&] (float t) { return left + span * t; };
    const auto tAt = [&] (float px) { return std::clamp ((px - left) / span, 0.0f, 1.0f); };
    ImGui::InvisibleButton ("##stops", ImVec2 (width, bar + arrows));
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    int selected = storage->GetInt (selectedId, 0);
    if (selected < 0 || selected >= int (stops.size ()) || stops[size_t (selected)].hint)
        selected = 0;
    bool changed = false;

    if (ImGui::IsItemActivated ()) {
        Kind kind = None;
        int index = -1;
        // A colour circle first, then a blend arrow, then the bar itself adds a colour.
        for (int i = 0; i < int (stops.size ()) && kind == None; ++i)
            if (!stops[size_t (i)].hint &&
                std::hypot (mouse.x - x (stops[size_t (i)].at), mouse.y - middle) <= radius + 3)
                kind = Colour, index = i;
        if (kind == None && mouse.y > at.y + bar)
            for (int i = 0; i < int (stops.size ()); ++i) {
                const int b = NextColour (stops, i);
                if (stops[size_t (i)].hint || b < 0)
                    continue;
                const int hint = HintBetween (stops, i, b);
                const float arrow =
                    hint >= 0 ? stops[size_t (hint)].at : 0.5f * (stops[size_t (i)].at + stops[size_t (b)].at);
                if (std::abs (mouse.x - x (arrow)) <= 7.0f)
                    kind = Blend, index = i;
            }
        if (kind == None && mouse.y <= at.y + bar) {
            index = AddColourStop (stops, tAt (mouse.x));
            if (index >= 0) {
                kind = Colour;
                changed = true;
            }
        }
        if (kind == Colour)
            selected = index;
        storage->SetInt (kindId, kind);
        storage->SetInt (indexId, index);
        storage->SetBool (movedId, kind == Colour && changed); // a new colour is not a click on it
    }
    const Kind kind = Kind (storage->GetInt (kindId, None));
    const int index = storage->GetInt (indexId, -1);
    // Dragged well off the bar, a colour is about to go -- if the ramp keeps two.
    const bool removing = ImGui::IsItemActive () && kind == Colour && index >= 0 && index < int (stops.size ()) &&
                          std::abs (mouse.y - middle) > 1.6f * bar && ColourStopCount (stops) > 2;
    if (ImGui::IsItemActive () && index >= 0 && index < int (stops.size ())) {
        if (std::abs (ImGui::GetIO ().MouseDelta.x) > 0.0f || std::abs (ImGui::GetIO ().MouseDelta.y) > 0.0f)
            if (ImGui::IsMouseDragging (ImGuiMouseButton_Left, 3.0f))
                storage->SetBool (movedId, true);
        if (kind == Colour && !removing && storage->GetBool (movedId))
            changed = MoveColourStop (stops, index, tAt (mouse.x)) || changed;
        else if (kind == Blend)
            changed = SetBlendMidpoint (stops, index, tAt (mouse.x)) || changed;
    }
    if (ImGui::IsItemDeactivated () && kind == Colour && index >= 0 && index < int (stops.size ())) {
        if (std::abs (mouse.y - middle) > 1.6f * bar && RemoveColourStop (stops, index)) {
            selected = (std::min) (selected, int (stops.size ()) - 1);
            changed = true;
        }
        else if (!storage->GetBool (movedId))
            OpenColourPopup ("##stop.colour", stops[size_t (index)].rgba); // a click: its colour
        storage->SetInt (kindId, None);
    }
    if (selected >= int (stops.size ()) || stops[size_t (selected)].hint)
        selected = 0;

    // The bar, the blend arrows under it, the colour circles on its middle.
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const float inset = 0.25f * bar;
    DrawSwatch (draw, ImVec2 (left, at.y + inset), ImVec2 (left + span, at.y + bar - inset), stops);
    draw->AddRect (ImVec2 (left, at.y + inset), ImVec2 (left + span, at.y + bar - inset),
                   ImGui::GetColorU32 (ImGuiCol_Border));
    for (int i = 0; i < int (stops.size ()); ++i) {
        const int b = NextColour (stops, i);
        if (stops[size_t (i)].hint || b < 0)
            continue;
        const int hint = HintBetween (stops, i, b);
        const float px = x (hint >= 0 ? stops[size_t (hint)].at : 0.5f * (stops[size_t (i)].at + stops[size_t (b)].at));
        const bool on = kind == Blend && index == i && ImGui::IsItemActive ();
        draw->AddTriangleFilled (ImVec2 (px, at.y + bar + 1.0f), ImVec2 (px - 5.0f, at.y + bar + arrows - 1.0f),
                                 ImVec2 (px + 5.0f, at.y + bar + arrows - 1.0f),
                                 on          ? IM_COL32_WHITE
                                 : hint >= 0 ? IM_COL32 (210, 210, 210, 255)
                                             : IM_COL32 (130, 130, 130, 255));
    }
    for (int i = 0; i < int (stops.size ()); ++i) {
        if (stops[size_t (i)].hint)
            continue;
        const uint32_t c = stops[size_t (i)].rgba;
        const ImU32 fill = IM_COL32 ((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, 255);
        ImVec2 centre (x (stops[size_t (i)].at), middle);
        const bool going = removing && i == index;
        if (going)
            centre = mouse; // follows the pointer off the bar; released there, it is removed
        draw->AddCircleFilled (centre, radius, fill);
        draw->AddCircle (centre, radius, going ? IM_COL32 (215, 48, 39, 255) : IM_COL32_WHITE, 0,
                         i == selected ? 3.0f : 1.5f);
        if (i == selected && !going)
            draw->AddCircle (centre, radius + 2.5f, IM_COL32 (30, 30, 30, 200), 0, 1.0f);
    }
    if (ImGui::IsItemHovered () && !ImGui::IsItemActive ())
        ImGui::SetTooltip ("Click a colour to change it, drag it to move it, drag it off the bar to remove it.\n"
                           "Click the bar to add a colour. Drag an arrow to move where two colours blend.");
    else if (removing)
        ImGui::SetTooltip ("Release to remove this colour");

    // The selected colour's own popup: opened by a click on its circle.
    if (ColourPopup ("##stop.colour", stops[size_t (selected)].rgba, true)) {
        RefreshHints (stops);
        changed = true;
    }
    storage->SetInt (selectedId, selected);
    ImGui::TextDisabled ("%d colours -- click a circle for its colour, drag it off to remove", ColourStopCount (stops));
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
