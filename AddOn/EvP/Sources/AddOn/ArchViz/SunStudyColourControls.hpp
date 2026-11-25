#pragma once
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/GradientPresetChoice.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include <imgui.h>

namespace geomsrv::archviz::viewerhud {
// One editable colour of a shadow view's palette (#116).
struct ShadowRole {
    uint32_t* rgba;
    uint32_t fallback; // the default, for "reset"
    const char* label;
    const char* tip;
};

// A shadow view's legend as pickers: a swatch per role that opens the colour
// picker, its word, and one "reset" when any differs from its default.
// DISPLAY ONLY, like the hours colours: no study reruns.
//
// ⚠️ EVERY ROLE IS ITS OWN ID SCOPE, under the view's. `##` hides a suffix but
// leaves the whole label in the ID, so a bare "##sunlit" here and one anywhere
// else in the window would collide -- as the view selector's "Roles" button
// and the run form's "Roles" node did.
inline bool DrawShadowPalette (const char* view, std::initializer_list<ShadowRole> roles)
{
    ImGui::PushID (view);
    bool edited = false;
    bool changed = false;
    int i = 0;
    for (const ShadowRole& role : roles) {
        if (i++ > 0)
            ImGui::SameLine ();
        ImGui::PushID (role.label);
        changed |= gradientchoice::ColourChoice ("##colour", *role.rgba, false);
        hudshell::ItemTip ("%s", role.tip);
        ImGui::PopID ();
        ImGui::SameLine ();
        ImGui::TextUnformatted (role.label);
        edited = edited || *role.rgba != role.fallback;
    }
    if (edited) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("reset")) {
            changed = true;
            for (const ShadowRole& role : roles)
                *role.rgba = role.fallback;
        }
        geomsrv::archviz::hudshell::ItemTip ("Puts this view's default colours back.");
    }
    ImGui::PopID ();
    return changed;
}

inline bool DrawHoursColourChoice (HudState& state, float width, bool inlineStyle)
{
    const bool changed =
        gradientchoice::GradientPresetChoice ("Colours##analysis", state.sunGradientPreset, state.sunGradientCustom,
                                              state.sunGradientFlipped, width, overlaylayers::GradientCategory::Sun);
    if (inlineStyle)
        ImGui::SameLine ();
    return ImGui::Checkbox ("Stepped gradient", &state.sunGradientStepped) || changed;
}

} // namespace geomsrv::archviz::viewerhud
