#include "ArchViz/HudShell.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include "ArchViz/GraphicsSettingsUi.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace geomsrv::archviz::hudshell {

namespace layers = overlaylayers;

void BaseStyle (float scale)
{
    ImGuiStyle& style = ImGui::GetStyle ();
    style = ImGuiStyle ();
    ImGui::StyleColorsDark (&style);
    // Commands share Tapioca's configured HUD; activating an editor must not
    // switch the global palette, spacing or user graphics overrides.
    style.FramePadding = ImVec2 (4.0f, 2.0f);
    style.ItemSpacing = ImVec2 (6.0f, 3.0f);
    style.ItemInnerSpacing = ImVec2 (4.0f, 3.0f);
    style.CellPadding = ImVec2 (4.0f, 1.0f);
    style.ScaleAllSizes (scale);
    style.ScrollbarSize /= 3.0f;
    style.WindowMinSize = ImVec2 (1.0f, 1.0f);
    style.FrameRounding = 2.0f * scale;
    graphicssettingsui::ApplyStyle (style, scale);
}

int PushLook (const layers::Panel& panel, float scale)
{
    ImGui::PushStyleVar (
        ImGuiStyleVar_WindowPadding,
        ImVec2 (float (graphicssettings::Number ("ui.size.WindowPadding.x", panel.paddingPixels)) * scale,
                float (graphicssettings::Number ("ui.size.WindowPadding.y", panel.paddingPixels)) * scale));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding,
                         float (graphicssettings::Number ("ui.size.WindowRounding", panel.roundingPixels)) * scale);
    ImGui::PushStyleVar (
        ImGuiStyleVar_WindowBorderSize,
        float (graphicssettings::Number ("ui.size.WindowBorderSize", (panel.borderRgba & 0xFFu) != 0 ? 1.0f : 0.0f)) *
            scale);
    const uint32_t text = panel.textRgba;
    const uint32_t accent = panel.accentRgba;
    const std::pair<ImGuiCol, uint32_t> colours[] = {
        { ImGuiCol_WindowBg, panel.backgroundRgba },
        { ImGuiCol_Border, panel.borderRgba },
        { ImGuiCol_Text, text },
        { ImGuiCol_Separator, WithAlpha (text, 0.3f) },
        { ImGuiCol_FrameBg, WithAlpha (text, 0.12f) },
        { ImGuiCol_TableHeaderBg, WithAlpha (text, 0.12f) },
        { ImGuiCol_TableBorderLight, WithAlpha (text, 0.2f) },
        { ImGuiCol_TableBorderStrong, WithAlpha (text, 0.3f) },
        { ImGuiCol_TableRowBgAlt, WithAlpha (text, 0.05f) },
        // ⚠️ WHAT THE POINTER CAN PRESS IS TINTED WITH THE ACCENT (the user, 2026-09-29): a
        // button reads as one at rest -- a faint fill -- and plainly when pointed at and
        // pressed, as a section's row does.
        { ImGuiCol_Header, WithAlpha (accent, 0.14f) }, // a dropdown's chosen option
        { ImGuiCol_HeaderHovered, WithAlpha (accent, 0.16f) },
        { ImGuiCol_HeaderActive, WithAlpha (accent, 0.28f) },
        { ImGuiCol_Button, WithAlpha (text, 0.07f) },
        { ImGuiCol_ButtonHovered, WithAlpha (accent, 0.30f) },
        { ImGuiCol_ButtonActive, WithAlpha (accent, 0.48f) },
        { ImGuiCol_FrameBgHovered, WithAlpha (accent, 0.20f) },
        { ImGuiCol_FrameBgActive, WithAlpha (accent, 0.32f) },
        { ImGuiCol_CheckMark, accent },
        { ImGuiCol_SliderGrab, accent },
        { ImGuiCol_SliderGrabActive, accent },
        // A tab bar: the tab shown tinted, a line of the accent over it. The same whether
        // ImGui thinks the panel focused or not -- the HUD takes no focus the user sees.
        { ImGuiCol_Tab, WithAlpha (text, 0.06f) },
        { ImGuiCol_TabHovered, WithAlpha (accent, 0.30f) },
        { ImGuiCol_TabSelected, WithAlpha (accent, 0.20f) },
        { ImGuiCol_TabSelectedOverline, accent },
        { ImGuiCol_TabDimmed, WithAlpha (text, 0.06f) },
        { ImGuiCol_TabDimmedSelected, WithAlpha (accent, 0.20f) },
        { ImGuiCol_TabDimmedSelectedOverline, accent },
        // Its tooltips in its own colours, nearly opaque over the model.
        { ImGuiCol_PopupBg, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u },
    };
    for (const auto& colour : colours)
        ImGui::PushStyleColor (colour.first, graphicssettingsui::StyleColour (colour.first, Colour (colour.second)));
    return int (sizeof (colours) / sizeof (colours[0]));
}

} // namespace geomsrv::archviz::hudshell
