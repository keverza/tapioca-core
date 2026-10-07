#include "ArchViz/GraphicsSettingsUi.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include "ArchViz/HudShell.hpp"
#include <cmath>
#include <limits>

namespace geomsrv::archviz::graphicssettingsui {
namespace gs = graphicssettings;
namespace {
uint32_t Packed (ImVec4 colour)
{
    return hudshell::Unpacked (ImGui::ColorConvertFloat4ToU32 (colour));
}
std::string ColourKey (ImGuiCol index)
{
    return std::string ("ui.color.") + ImGui::GetStyleColorName (index);
}
void Number (const char* name, float& value, float scale, float min = 0, float max = 40,
             const char* units = "logical px")
{
    const auto key = std::string ("ui.size.") + name;
    gs::Register ({ key, "UI sizes and spacing", units, gs::Kind::Number, value / scale, min, max });
    value = float (gs::Number (key, value / scale)) * scale;
}
} // namespace
ImVec4 StyleColour (ImGuiCol index, ImVec4 fallback)
{
    const auto key = ColourKey (index);
    gs::Register (
        { key, "UI colours and interactions", "RGBA", gs::Kind::Colour, double (Packed (fallback)), 0, 4294967295.0 });
    return hudshell::Colour (gs::Colour (key, Packed (fallback)));
}
void ApplyStyle (ImGuiStyle& style, float scale)
{
    if (!std::isfinite (scale) || scale <= 0)
        return;
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        style.Colors[i] = StyleColour (i, style.Colors[i]);
#define SIZE(field) Number (#field, style.field, scale)
#define PAIR(field)                                                                                                    \
    Number (#field ".x", style.field.x, scale);                                                                        \
    Number (#field ".y", style.field.y, scale)
    PAIR (WindowPadding);
    PAIR (FramePadding);
    PAIR (CellPadding);
    PAIR (ItemSpacing);
    PAIR (ItemInnerSpacing);
    PAIR (TouchExtraPadding);
    PAIR (DisplayWindowPadding);
    PAIR (DisplaySafeAreaPadding);
    SIZE (WindowRounding);
    SIZE (ChildRounding);
    SIZE (FrameRounding);
    SIZE (PopupRounding);
    SIZE (ScrollbarSize);
    SIZE (ScrollbarRounding);
    SIZE (GrabMinSize);
    SIZE (GrabRounding);
    SIZE (LogSliderDeadzone);
    SIZE (WindowBorderHoverPadding);
    SIZE (ImageBorderSize);
    SIZE (TabBarBorderSize);
    SIZE (TabBarOverlineSize);
    SIZE (TreeLinesSize);
    SIZE (TreeLinesRounding);
    SIZE (WindowBorderSize);
    SIZE (ChildBorderSize);
    SIZE (PopupBorderSize);
    SIZE (FrameBorderSize);
    SIZE (IndentSpacing);
    SIZE (ColumnsMinSpacing);
    SIZE (TabRounding);
    SIZE (TabBorderSize);
    SIZE (SeparatorTextBorderSize);
    PAIR (SeparatorTextPadding);
#undef PAIR
#undef SIZE
    Number ("Alpha", style.Alpha, 1, 0.1f, 1, "opacity multiplier");
    Number ("DisabledAlpha", style.DisabledAlpha, 1, 0.1f, 1, "opacity multiplier");
    Number ("HoverStationaryDelay", style.HoverStationaryDelay, 1, 0, 2, "seconds");
    Number ("HoverDelayShort", style.HoverDelayShort, 1, 0, 2, "seconds");
    Number ("HoverDelayNormal", style.HoverDelayNormal, 1, 0, 2, "seconds");
    Number ("ButtonTextAlign.x", style.ButtonTextAlign.x, 1, 0, 1, "fraction");
    Number ("ButtonTextAlign.y", style.ButtonTextAlign.y, 1, 0, 1, "fraction");
    Number ("SelectableTextAlign.x", style.SelectableTextAlign.x, 1, 0, 1, "fraction");
    Number ("SelectableTextAlign.y", style.SelectableTextAlign.y, 1, 0, 1, "fraction");
    Number ("WindowTitleAlign.x", style.WindowTitleAlign.x, 1, 0, 1, "fraction");
    Number ("WindowTitleAlign.y", style.WindowTitleAlign.y, 1, 0, 1, "fraction");
    Number ("SeparatorTextAlign.x", style.SeparatorTextAlign.x, 1, 0, 1, "fraction");
    Number ("SeparatorTextAlign.y", style.SeparatorTextAlign.y, 1, 0, 1, "fraction");
    Number ("TableAngledHeadersTextAlign.x", style.TableAngledHeadersTextAlign.x, 1, 0, 1, "fraction");
    Number ("TableAngledHeadersTextAlign.y", style.TableAngledHeadersTextAlign.y, 1, 0, 1, "fraction");
    Number ("MouseCursorScale", style.MouseCursorScale, 1, 0.5f, 3, "multiplier");
    Number ("CurveTessellationTol", style.CurveTessellationTol, 1, 0.1f, 10);
    Number ("CircleTessellationMaxError", style.CircleTessellationMaxError, 1, 0.1f, 2);
    const auto flag = [] (const char* name, bool& value) {
        const auto key = std::string ("ui.render.") + name;
        gs::Register ({ key, "UI rendering", {}, gs::Kind::Boolean, value ? 1.0 : 0.0, 0, 1 });
        value = gs::Number (key, value ? 1 : 0) != 0;
    };
    flag ("AntiAliasedLines", style.AntiAliasedLines);
    flag ("AntiAliasedLinesUseTex", style.AntiAliasedLinesUseTex);
    flag ("AntiAliasedFill", style.AntiAliasedFill);
}

void Draw (const std::function<void ()>& displayStyles)
{
    if (!hudshell::Section ("Graphics style lab", false))
        return;
    ImGui::PushID ("graphics-style-lab");
    if (displayStyles)
        displayStyles ();
    ImGui::TextWrapped ("Live category overrides. Unchecked rows keep authored styles; their values are reference "
                        "seeds. Export to logs for review, not automatic defaults.");
    static thread_local uint64_t savedRevision = std::numeric_limits<uint64_t>::max ();
    static thread_local std::string status;
    auto settings = gs::Current ();
    const bool dirty = settings->revision != savedRevision;
    ImGui::BeginDisabled (!dirty);
    if (ImGui::Button ("Save to logs")) {
        const auto result = gs::SaveToLogs (*settings);
        if (result.saved) {
            savedRevision = settings->revision;
            const auto path = result.path.u8string ();
            status.assign (path.begin (), path.end ());
        }
        else
            status = "Save failed: " + result.error;
    }
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::Button ("Reset overrides"))
        gs::Reset ();
    if (!status.empty ())
        ImGui::TextWrapped ("%s", status.c_str ());
    if (ImGui::Button ("Apply taste UI palette")) {
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            const std::string name = ImGui::GetStyleColorName (i);
            uint32_t rgba = gs::Palette ()[2].rgba;
            if (name == "TextDisabled")
                rgba = gs::Palette ()[7].rgba;
            else if (name == "Text")
                rgba = gs::Palette ()[1].rgba;
            else if (name.find ("Border") != std::string::npos || name.find ("Separator") != std::string::npos)
                rgba = gs::Palette ()[3].rgba;
            else if (name == "CheckMark" || name.find ("Grab") != std::string::npos ||
                     name.find ("Overline") != std::string::npos || name == "TextLink")
                rgba = gs::Palette ()[4].rgba;
            else if (name.find ("Active") != std::string::npos || name.find ("Selected") != std::string::npos)
                rgba = hudshell::WithAlpha (gs::Palette ()[4].rgba, 0.28f);
            else if (name.find ("Hovered") != std::string::npos)
                rgba = hudshell::WithAlpha (gs::Palette ()[4].rgba, 0.20f);
            else if (name == "WindowBg" || name == "PopupBg")
                rgba = gs::Palette ()[0].rgba;
            gs::Set (ColourKey (i), rgba);
        }
    }
    settings = gs::Current ();
    std::map<std::string, std::vector<std::string>> groups;
    for (const auto& [key, definition] : settings->definitions)
        groups[definition.group].push_back (key);
    for (const auto& [group, keys] : groups) {
        if (!ImGui::TreeNode (group.c_str ()))
            continue;
        for (const auto& key : keys) {
            ImGui::PushID (key.c_str ());
            const auto& d = settings->definitions.at (key);
            const auto& v = settings->values.at (key);
            bool enabled = v.enabled;
            if (ImGui::Checkbox ("##override", &enabled)) {
                if (enabled)
                    gs::Set (key, v.number);
                else
                    gs::Reset (key);
            }
            ImGui::SameLine ();
            ImGui::TextUnformatted (key.c_str ());
            if (!d.units.empty ())
                hudshell::Tip (d.units);
            ImGui::SetNextItemWidth (-FLT_MIN);
            double number = v.number;
            if (d.kind == gs::Kind::Colour) {
                auto colour = hudshell::Colour (uint32_t (number));
                if (ImGui::ColorEdit4 ("##value", &colour.x,
                                       ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
                    gs::Set (key, Packed (colour));
                if (ImGui::BeginCombo ("##palette", "Palette")) {
                    for (const auto& swatch : gs::Palette ()) {
                        ImGui::PushID (swatch.name.c_str ());
                        const bool pickedSwatch = ImGui::ColorButton ("##swatch", hudshell::Colour (swatch.rgba),
                                                                      ImGuiColorEditFlags_NoTooltip, { 12, 12 });
                        ImGui::SameLine ();
                        if (ImGui::Selectable (swatch.name.c_str ()) || pickedSwatch) {
                            gs::Set (key, swatch.rgba);
                            ImGui::CloseCurrentPopup ();
                        }
                        ImGui::PopID ();
                    }
                    ImGui::EndCombo ();
                }
            }
            else if (d.kind == gs::Kind::Boolean) {
                bool checked = number != 0;
                if (ImGui::Checkbox ("On##value", &checked))
                    gs::Set (key, checked ? 1 : 0);
            }
            else if (ImGui::SliderScalar ("##value", ImGuiDataType_Double, &number, &d.min, &d.max, "%.3f",
                                          ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
                gs::Set (key, number);
            ImGui::PopID ();
        }
        ImGui::TreePop ();
    }
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::graphicssettingsui
