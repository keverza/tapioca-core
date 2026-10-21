#include "ArchViz/HudShell.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include "ArchViz/GraphicsSettingsUi.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace geomsrv::archviz::hudshell {

namespace layers = overlaylayers;

namespace {

// How light `rgba` reads, 0 to 1, its alpha ignored.
float Luminance (uint32_t rgba)
{
    const float r = float ((rgba >> 24) & 0xFFu), g = float ((rgba >> 16) & 0xFFu), b = float ((rgba >> 8) & 0xFFu);
    return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
}

// `from` moved `toward` of the way to `to`, every channel alpha included.
uint32_t Mixed (uint32_t from, uint32_t to, float toward)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float a = float ((from >> shift) & 0xFFu), b = float ((to >> shift) & 0xFFu);
        out |= uint32_t (std::lround (a + (b - a) * toward)) << shift;
    }
    return out;
}

} // namespace

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
    // ⚠️ A NEUTRAL FILL IS THE CARD'S OWN INK, AND STRONGER ON A DARK CARD (the user, 2026-10-21:
    // on the dark card the radio circles, the small buttons and the muted text could not be read).
    // Light ink at the alpha that reads on a near-white card all but vanishes on a dark one.
    const bool dark = Luminance (panel.backgroundRgba) < 0.5f;
    const auto ink = [text, dark] (float alpha) {
        return WithAlpha (text, (std::min) (alpha * (dark ? 1.6f : 1.0f), 1.0f));
    };
    // A title bar is drawn over the model, not over the card: opaque, a step from the ground.
    const auto bar = [&] (float toward) { return Mixed (panel.backgroundRgba | 0xFFu, text | 0xFFu, toward); };
    const std::pair<ImGuiCol, uint32_t> colours[] = {
        { ImGuiCol_WindowBg, panel.backgroundRgba },
        { ImGuiCol_Border, panel.borderRgba },
        { ImGuiCol_Text, text },
        { ImGuiCol_TextDisabled, WithAlpha (text, dark ? 0.62f : 0.55f) },
        { ImGuiCol_TitleBg, bar (0.10f) },
        { ImGuiCol_TitleBgActive, bar (0.14f) },
        { ImGuiCol_TitleBgCollapsed, bar (0.10f) },
        { ImGuiCol_Separator, WithAlpha (text, 0.3f) },
        { ImGuiCol_FrameBg, ink (0.12f) },
        { ImGuiCol_TableHeaderBg, ink (0.12f) },
        { ImGuiCol_TableBorderLight, WithAlpha (text, 0.2f) },
        { ImGuiCol_TableBorderStrong, WithAlpha (text, 0.3f) },
        { ImGuiCol_TableRowBgAlt, ink (0.05f) },
        // ⚠️ SECTIONS AND TABS ARE GREY, NOT BLUE (the user, 2026-10-21): a section's row, a
        // dropdown's chosen option and every tab are the card's ink at rest, pointed at and
        // pressed. The accent stays where it says something: the shown tab's line, a check, a
        // slider's grip, and a button or field under the pointer (the user, 2026-09-29).
        { ImGuiCol_Header, ink (0.10f) },
        { ImGuiCol_HeaderHovered, ink (0.16f) },
        { ImGuiCol_HeaderActive, ink (0.24f) },
        { ImGuiCol_Button, ink (0.08f) },
        { ImGuiCol_ButtonHovered, WithAlpha (accent, 0.30f) },
        { ImGuiCol_ButtonActive, WithAlpha (accent, 0.48f) },
        { ImGuiCol_FrameBgHovered, WithAlpha (accent, 0.20f) },
        { ImGuiCol_FrameBgActive, WithAlpha (accent, 0.32f) },
        { ImGuiCol_CheckMark, accent },
        { ImGuiCol_SliderGrab, accent },
        { ImGuiCol_SliderGrabActive, accent },
        // A tab bar: the tab shown a step greyer, a line of the accent over it. The same whether
        // ImGui thinks the panel focused or not -- the HUD takes no focus the user sees.
        { ImGuiCol_Tab, ink (0.06f) },
        { ImGuiCol_TabHovered, ink (0.14f) },
        { ImGuiCol_TabSelected, ink (0.16f) },
        { ImGuiCol_TabSelectedOverline, accent },
        { ImGuiCol_TabDimmed, ink (0.06f) },
        { ImGuiCol_TabDimmedSelected, ink (0.16f) },
        { ImGuiCol_TabDimmedSelectedOverline, accent },
        // Its tooltips in its own colours, nearly opaque over the model.
        { ImGuiCol_PopupBg, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u },
    };
    for (const auto& colour : colours)
        ImGui::PushStyleColor (colour.first, graphicssettingsui::StyleColour (colour.first, Colour (colour.second)));
    return int (sizeof (colours) / sizeof (colours[0]));
}

} // namespace geomsrv::archviz::hudshell
