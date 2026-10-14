#include "ArchViz/HudShell.hpp"
#include "ArchViz/GraphicsSettingsUi.hpp"

namespace geomsrv::archviz::hudshell {
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
} // namespace geomsrv::archviz::hudshell
