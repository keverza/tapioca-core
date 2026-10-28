// ArchViz/DiligentHudControls -- the Analysis page's own small controls: a
// choice among a few options, a slider whose range follows its value, and a
// colour swatch with a picker. Declared in DiligentHud.hpp.

#include "ArchViz/DiligentHud.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace geomsrv::archviz::viewerhud {

bool Choice (const char* label, int& value, const char* const options[], int count)
{
    // A dropdown hides what it offers; a few options are shown outright. Buttons
    // while every label is short, a list box once one would make the row wrap.
    constexpr int kButtonsBelow = 4;
    constexpr float kButtonLabelEms = 9.0f;
    if (count >= kButtonsBelow)
        return ImGui::Combo (label, &value, options, count);
    float widest = 0.0f;
    for (int i = 0; i < count; ++i)
        widest = (std::max) (widest, ImGui::CalcTextSize (options[i]).x);
    bool changed = false;
    ImGui::PushID (label);
    if (widest <= kButtonLabelEms * ImGui::GetFontSize ()) {
        for (int i = 0; i < count; ++i) {
            if (i > 0)
                ImGui::SameLine (0.0f, 2.0f);
            const bool on = value == i;
            if (on)
                ImGui::PushStyleColor (ImGuiCol_Button, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
            if (ImGui::Button (options[i]) && !on) {
                value = i;
                changed = true;
            }
            if (on)
                ImGui::PopStyleColor ();
        }
        const char* hidden = std::strstr (label, "##");
        if (hidden != label) {
            ImGui::SameLine ();
            ImGui::TextUnformatted (label, hidden);
        }
    }
    else {
        const char* hidden = std::strstr (label, "##");
        if (hidden != label)
            ImGui::TextUnformatted (label, hidden);
        const int previous = value;
        ImGui::SetNextItemWidth (-FLT_MIN);
        if (ImGui::BeginListBox ("##choices", ImVec2 (-FLT_MIN, float (count) * ImGui::GetTextLineHeightWithSpacing () +
                                                                    ImGui::GetStyle ().FramePadding.y * 2.0f))) {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable (options[i], value == i))
                    value = i;
            ImGui::EndListBox ();
        }
        changed = value != previous;
    }
    ImGui::PopID ();
    return changed;
}

double AdaptiveSliderRange (double value)
{
    // The smallest 1, 2 or 5 x 10^k at least twice the value: 20 -> 50, 50 -> 100.
    const double wanted = (std::max) (2.0 * std::abs (value), 1.0e-9);
    double decade = std::pow (10.0, std::floor (std::log10 (wanted)));
    for (;;) {
        for (const double step : { 1.0, 2.0, 5.0 })
            if (step * decade >= wanted * (1.0 - 1.0e-12))
                return step * decade;
        decade *= 10.0;
    }
}

bool AdaptiveSlider (const char* label, double& value, double& range, bool& held, double minimum, double maximum,
                     const char* format)
{
    // The range is re-chosen only when no drag is under way, so it never jumps
    // out from under the cursor; a value typed past it (ctrl+click) widens it.
    if (!held && (!(range > 0.0) || value > range))
        range = (std::min) (maximum, AdaptiveSliderRange (value));
    const double low = 0.0;
    const bool changed = ImGui::SliderScalar (label, ImGuiDataType_Double, &value, &low, &range, format);
    held = ImGui::IsItemActive ();
    if (ImGui::IsItemDeactivatedAfterEdit ())
        range = (std::min) (maximum, AdaptiveSliderRange (value));
    value = (std::max) (minimum, (std::min) (maximum, value));
    return changed;
}

bool ColourPicker (const char* label, uint32_t& rgba, bool alpha)
{
    float colour[4] = { float ((rgba >> 24) & 0xffu) / 255.0f, float ((rgba >> 16) & 0xffu) / 255.0f,
                        float ((rgba >> 8) & 0xffu) / 255.0f, float (rgba & 0xffu) / 255.0f };
    const ImGuiColorEditFlags flags =
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoDragDrop |
        (alpha ? ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha);
    if (!ImGui::ColorEdit4 (label, colour, flags))
        return false;
    rgba = 0;
    for (int channel = 0; channel < 4; ++channel)
        rgba = (rgba << 8) | uint32_t (std::lround ((std::min) (1.0f, (std::max) (0.0f, colour[channel])) * 255.0f));
    return true;
}

} // namespace geomsrv::archviz::viewerhud
