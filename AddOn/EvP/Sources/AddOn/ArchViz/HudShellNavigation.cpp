// Explicit navigation only: this first-pass command bar never executes model commands.
// OVERLAY-INVARIANTS.md: render snapshots without changing viewport input ownership.
#include "ArchViz/HudShell.hpp"
#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace geomsrv::archviz::hudshell {
namespace {
std::string Fold (std::string value)
{
    std::transform (value.begin (), value.end (), value.begin (),
                    [] (unsigned char c) { return char (std::tolower (c)); });
    const auto first = value.find_first_not_of (" \t");
    if (first == std::string::npos)
        return {};
    return value.substr (first, value.find_last_not_of (" \t") - first + 1);
}
bool Global (const std::string& key)
{
    return key == kSelectionKey || key == kSettingsKey || key == kDebugKey || key == kStatsKey;
}
} // namespace

std::string ResolveNavigation (const std::string& input, const std::vector<HostTab>& tabs)
{
    std::string query = Fold (input);
    for (const std::string prefix : { "workflow ", "show " })
        if (query.rfind (prefix, 0) == 0) {
            query = Fold (query.substr (prefix.size ()));
            break;
        }
    std::string found;
    for (const auto& tab : tabs)
        if (Fold (tab.key) == query || Fold (tab.title) == query ||
            (!tab.token.empty () && Fold (tab.token) == query)) {
            if (!found.empty () && found != tab.key)
                return {};
            found = tab.key;
        }
    return found;
}

std::string NavigationHeader (const HostSpec& spec, const std::string& held, NavigationState& state)
{
    std::string selected;
    const HostTab* current = nullptr;
    const HostTab* workflow = nullptr;
    for (const auto& tab : spec.tabs) {
        if (tab.key == held)
            current = &tab;
        if (tab.key == state.workflow)
            workflow = &tab;
    }
    if (current && !Global (held)) {
        state.workflow = held;
        state.workflowTitle = current->title;
        workflow = current;
    }
    if (workflow) {
        ImGui::PushID (workflow->key.c_str ());
        if (ui::IconTextButton ("##workflow", workflow->icon, workflow->title.c_str (), workflow->key == held))
            selected = workflow->key;
        ImGui::PopID ();
        if (ImGui::GetContentRegionAvail ().x > 90 * spec.ui)
            ImGui::SameLine ();
    }
    for (const auto& tab : spec.tabs) {
        if (!Global (tab.key))
            continue;
        ImGui::PushID (tab.key.c_str ());
        std::string label = tab.title + (tab.badge.empty () ? "" : " (" + tab.badge + ")");
        const auto icon = tab.key == kSelectionKey  ? ui::IconId::CursorPointer
                          : tab.key == kSettingsKey ? ui::IconId::Settings
                          : tab.key == kDebugKey    ? ui::IconId::Bug
                                                    : ui::IconId::ReportColumns;
        if (ui::IconTextButton ("##utility", icon, label.c_str (), tab.key == held))
            selected = tab.key;
        ImGui::PopID ();
        if (ImGui::GetContentRegionAvail ().x > 90 * spec.ui)
            ImGui::SameLine ();
    }
    ImGui::NewLine ();
    return selected;
}

std::string NavigationInput (const HostSpec& spec, NavigationState& state, bool& active)
{
    std::string selected;
    ImGui::Separator ();
    ImGui::SetNextItemWidth (-1);
    ImGui::BeginDisabled (!spec.textInput);
    const char* hint =
        spec.textInput ? "show Settings / workflow <name>" : "Keyboard commands unavailable; use dock tab";
    const bool submit =
        ImGui::InputTextWithHint ("##hudCommand", hint, state.input, sizeof state.input,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll);
    active = ImGui::IsItemActive ();
    ImGui::EndDisabled ();
    Tip (spec.textInput ? "Navigation commands only in this pass. Use the exact page name."
                        : "Keyboard input is not available in this viewer; use the dock tab.");
    if (submit) {
        selected = ResolveNavigation (state.input, spec.tabs);
        state.message = selected.empty () ? "No unique page matches. Use its exact name from the dock list." : "";
        if (!selected.empty ())
            state.input[0] = '\0';
    }
    ErrorNotice (state.message, spec.ui);
    return selected;
}
} // namespace geomsrv::archviz::hudshell
