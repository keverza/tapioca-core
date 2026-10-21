// ArchViz/HudShellTabs -- see the header.

#include "ArchViz/HudShellTabs.hpp"

#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"

#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {
namespace hudshell {

namespace {

// ⚠️ THE HUD'S OWN TABS ARE GLYPHS (HANDOFF-TapiocaUI-Library.md section 7.8; the user, 2026-10-21):
// Stats the report columns, Selection the pointer, Settings the gear, Debug the bug; a caller's tab
// is a glyph when it names one (HostTab::icon) and keeps its title otherwise. The title is still
// the tab's identity and is said as its tip; Debug's unseen count is its badge.
ui::IconId IconOf (const HostTab& tab)
{
    if (tab.icon != ui::IconId::None)
        return tab.icon;
    const std::string& key = tab.key;
    if (key == kStatsKey)
        return ui::IconId::ReportColumns;
    if (key == kSelectionKey)
        return ui::IconId::CursorPointer;
    if (key == kSettingsKey)
        return ui::IconId::Settings;
    if (key == kDebugKey)
        return ui::IconId::Bug;
    return ui::IconId::None;
}

// A glyph a little taller than the tab's text, whole pixels.
float TabGlyph ()
{
    return std::round (ImGui::GetFontSize () * 1.2f);
}

} // namespace

float TabWidth (const HostTab& tab)
{
    const ImGuiStyle& style = ImGui::GetStyle ();
    if (IconOf (tab) == ui::IconId::None)
        return ImGui::CalcTextSize (tab.title.c_str ()).x + 2.0f * style.FramePadding.x;
    float width = 2.0f * style.FramePadding.x + TabGlyph ();
    if (!tab.badge.empty ())
        width += style.ItemInnerSpacing.x + ImGui::CalcTextSize (tab.badge.c_str ()).x;
    return width;
}

bool TabItem (const HostTab& tab, ImGuiTabItemFlags flags)
{
    const ImGuiStyle& style = ImGui::GetStyle ();
    const ui::IconId icon = IconOf (tab);
    if (icon != ui::IconId::None)
        ImGui::SetNextItemWidth (TabWidth (tab));
    const std::string label = (icon != ui::IconId::None ? std::string () : tab.title) + "###" + tab.key;
    const bool open = ImGui::BeginTabItem (label.c_str (), nullptr, flags | ImGuiTabItemFlags_NoTooltip);
    if (icon != ui::IconId::None) {
        const ImVec2 min = ImGui::GetItemRectMin (), max = ImGui::GetItemRectMax ();
        const float glyph = TabGlyph (), middle = (min.y + max.y) * 0.5f;
        const float left = min.x + style.FramePadding.x;
        ImDrawList* const draw = ImGui::GetWindowDrawList ();
        ui::DrawIcon (draw, icon, ImVec2 (left + glyph * 0.5f, middle), glyph, ui::IconInk (open));
        if (!tab.badge.empty ())
            draw->AddText (ImVec2 (left + glyph + style.ItemInnerSpacing.x, middle - ImGui::GetFontSize () * 0.5f),
                           ImGui::GetColorU32 (ImGuiCol_Text), tab.badge.c_str ());
        Tip (tab.badge.empty () ? tab.title : tab.title + " (" + tab.badge + ")", TipSide::Below);
    }
    return open;
}

} // namespace hudshell
} // namespace archviz
} // namespace geomsrv
