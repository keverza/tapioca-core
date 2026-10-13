#include "ArchViz/HudShell.hpp"
#include "ArchViz/GraphicsSettingsUi.hpp"

namespace geomsrv::archviz::hudshell {
void BaseStyle (float scale, bool defaults)
{
    ImGuiStyle& style = ImGui::GetStyle ();
    style = ImGuiStyle ();
    ImGui::StyleColorsDark (&style);
    if (defaults) {
        style.ScaleAllSizes (scale);
        return;
    }
    // Existing tools retain their explicitly configured compact HUD. Roof editing
    // uses upstream ImGui defaults; its document palette is canvas furniture only.
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
