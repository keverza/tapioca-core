// The viewport panel's "sun study" section -- the web study's hours-range filter
// and legend, beside the model they describe.
//
// ⚠️ ITS OWN TRANSLATION UNIT because DiligentHud.cpp sits at the size cap, and
// because this is one concern: what the tint on screen means, and which part of
// it is shown. AnnotationHudControls is the precedent.
//
// RENDER THREAD, like the rest of the HUD. It reads the scene's overlay status
// (what the renderer is drawing) and writes HudState; the frame loop hands the
// range to DiligentScene::SetSunStudyFilter.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyOverlay.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

// The web study's SUN_COLORS (Commands/SunStudy/sunpalette.py), sRGB, as the
// tint shader's kSunBins. ⚠️ THE SAME TEN, OR THE LEGEND LIES ABOUT THE MODEL.
constexpr unsigned kSunBinColours[10] = { 0x6b3d18, 0x8a4f1f, 0x9c5a23, 0xb06a28, 0xc07d33,
                                          0xcf8f44, 0xdca157, 0xe7b674, 0xf0cb96, 0xf7e3c2 };
constexpr const char* kSunBinLabels[10] = { "0 - 1 h", "1 - 2 h", "2 - 3 h", "3 - 4 h", "4 - 5 h",
                                            "5 - 6 h", "6 - 7 h", "7 - 8 h", "8 - 9 h", "9+ h" };

ImVec4 Rgb (unsigned hex)
{
    return ImVec4 (float ((hex >> 16) & 0xff) / 255.0f, float ((hex >> 8) & 0xff) / 255.0f, float (hex & 0xff) / 255.0f,
                   1.0f);
}

// The web slider's step. Finer than any timestep a study is run at, so a range
// edge can sit exactly on a value the study can report.
float Snap (float hours)
{
    return std::round (hours * 4.0f) / 4.0f;
}

void Swatch (const char* id, const ImVec4& colour, const char* tooltip)
{
    ImGui::ColorButton (id, colour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                        ImVec2 (16.0f, 16.0f));
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("%s", tooltip);
}

} // namespace

void DrawSunStudyHudSection (HudState& state, const DiligentSceneStats& scene)
{
    const SunStudyOverlayStatus& study = scene.sunStudy;
    // ⚠️ ONLY WHILE A STUDY IS ON SCREEN. A range slider with nothing to filter
    // reads as a control that does nothing.
    if (!study.drawing)
        return;
    if (!ImGui::CollapsingHeader ("sun study", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled ("%s, %u element(s)", study.studyId.c_str (), unsigned (study.elementsAttached));

    // ---- the hours range ------------------------------------------------------
    //
    // The compliance question -- "which surfaces get more than 2.5 h" -- is a
    // RANGE on the value, not a bin of the colour scale, so the two ends move
    // independently and in quarter hours. The colours stay binned; the
    // selection does not.
    const float top = kSunHoursFilterOpenTop;
    ImGui::SetNextItemWidth (-60.0f);
    ImGui::SliderFloat ("from##sunlo", &state.sunFilterLo, 0.0f, top, "%.2f h");
    ImGui::SetNextItemWidth (-60.0f);
    ImGui::SliderFloat ("to##sunhi", &state.sunFilterHi, 0.0f, top, state.sunFilterHi >= top ? "9+ h" : "%.2f h");
    state.sunFilterLo = Snap (std::clamp (state.sunFilterLo, 0.0f, top));
    state.sunFilterHi = Snap (std::clamp (state.sunFilterHi, 0.0f, top));
    // Crossing ends is a person dragging one past the other; the one that
    // moved pushes, the way the web page's pair behaves.
    if (state.sunFilterLo > state.sunFilterHi)
        state.sunFilterHi = state.sunFilterLo;
    ImGui::Checkbox ("hide the rest", &state.sunFilterHide);
    if (state.sunFilterLo > 0.0f || state.sunFilterHi < top) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("all")) {
            state.sunFilterLo = 0.0f;
            state.sunFilterHi = top;
        }
    }

    // ---- the legend -----------------------------------------------------------
    ImGui::TextDisabled ("direct sun hours");
    for (int bin = 0; bin < 10; ++bin) {
        if (bin > 0)
            ImGui::SameLine (0.0f, 2.0f);
        ImGui::PushID (bin);
        Swatch ("##sunbin", Rgb (kSunBinColours[bin]), kSunBinLabels[bin]);
        ImGui::PopID ();
    }
    ImGui::TextDisabled ("0 h %*s 9+ h", 22, "");

    // The role view's three colours -- the tint shader's RoleColor.
    ImGui::TextDisabled ("roles view");
    Swatch ("##roleA", Rgb (0xf59e24), "analysis: measured");
    ImGui::SameLine ();
    ImGui::TextUnformatted ("analysis");
    ImGui::SameLine ();
    Swatch ("##roleC", Rgb (0x8599b3), "context: casts shadow, not measured");
    ImGui::SameLine ();
    ImGui::TextUnformatted ("context");
    ImGui::SameLine ();
    Swatch ("##roleI", Rgb (0xd65cb8), "ignored: absent from the study");
    ImGui::SameLine ();
    ImGui::TextUnformatted ("ignored");
}

} // namespace archviz
} // namespace geomsrv
