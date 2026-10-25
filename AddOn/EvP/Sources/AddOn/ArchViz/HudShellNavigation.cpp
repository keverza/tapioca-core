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
WorkflowFamily FamilyOf (const HostTab& tab)
{
    return Global (tab.key) ? WorkflowFamily::Global : tab.family;
}
const char* FamilyName (WorkflowFamily family)
{
    switch (family) {
        case WorkflowFamily::Analyze:
            return "Analyze";
        case WorkflowFamily::Design:
            return "Design";
        case WorkflowFamily::Document:
            return "Document";
        case WorkflowFamily::Other:
            return "Other workflows";
        case WorkflowFamily::Global:
            return "Pages";
    }
    return "Other workflows";
}

bool WorkflowSelector (const std::string& title, ui::IconId icon)
{
    const ImGuiStyle& style = ImGui::GetStyle ();
    const ImVec2 text = ImGui::CalcTextSize (title.c_str ());
    const float height = ImGui::GetFrameHeight ();
    const float glyph = std::round (height * 0.64f);
    const float arrow = std::round (height * 0.30f);
    const float iconSpace = icon == ui::IconId::None ? 0.0f : glyph + style.ItemInnerSpacing.x;
    const float width = style.FramePadding.x * 2.0f + iconSpace + text.x + style.ItemInnerSpacing.x + arrow;
    const bool pressed = ImGui::Button ("##workflowSelector", ImVec2 (width, height));
    const ImVec2 min = ImGui::GetItemRectMin (), max = ImGui::GetItemRectMax ();
    const ImU32 ink = ImGui::GetColorU32 (ImGuiCol_Text);
    ImDrawList* const draw = ImGui::GetWindowDrawList ();
    const float middle = std::round ((min.y + max.y) * 0.5f);
    const float left = std::round (min.x + style.FramePadding.x);
    if (icon != ui::IconId::None)
        ui::DrawIcon (draw, icon, ImVec2 (left + glyph * 0.5f, middle), glyph, ink);
    draw->AddText (ImVec2 (left + iconSpace, std::round (middle - text.y * 0.5f)), ink, title.c_str ());
    const float centre = std::round (max.x - style.FramePadding.x - arrow * 0.5f);
    const float half = std::round (arrow * 0.5f);
    // The bundled font lacks the Unicode down chevron; geometry is legible in both HUD hosts.
    draw->AddTriangleFilled (ImVec2 (centre - half, middle - 1.0f), ImVec2 (centre + half, middle - 1.0f),
                             ImVec2 (centre, middle + half), ink);
    return pressed;
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

std::string WorkflowMenu (const std::vector<HostTab>& tabs, const std::string& held)
{
    std::string selected;
    for (const auto family :
         { WorkflowFamily::Analyze, WorkflowFamily::Design, WorkflowFamily::Document, WorkflowFamily::Other }) {
        bool heading = false;
        std::string group;
        for (const auto& tab : tabs) {
            if (FamilyOf (tab) != family)
                continue;
            if (!heading) {
                ImGui::SeparatorText (FamilyName (family));
                heading = true;
            }
            if (!tab.group.empty () && tab.group != group) {
                group = tab.group;
                ImGui::TextDisabled ("%s", group.c_str ());
            }
            if (ImGui::MenuItem ((tab.title + "##" + tab.key).c_str (), nullptr, tab.key == held))
                selected = tab.key;
            if (!tab.purpose.empty ())
                Tip (tab.purpose);
        }
    }
    if (ImGui::BeginMenu ("Pages")) {
        for (const auto& tab : tabs)
            if (FamilyOf (tab) == WorkflowFamily::Global &&
                ImGui::MenuItem ((tab.title + "##" + tab.key).c_str (), nullptr, tab.key == held))
                selected = tab.key;
        ImGui::EndMenu ();
    }
    return selected;
}

std::string NavigationHeader (const HostSpec& spec, const std::string& held, NavigationState& state, bool& collapse)
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
    const std::string title = workflow ? workflow->title : "Choose workflow";
    if (WorkflowSelector (title, workflow ? workflow->icon : ui::IconId::None) ||
        ImGui::IsItemClicked (ImGuiMouseButton_Right))
        ImGui::OpenPopup ("##workflowMenu");
    const std::string action = "Choose a workflow; switching does not run a command.";
    Tip (workflow && !workflow->purpose.empty () ? action + " " + workflow->purpose : action);
    if (ImGui::BeginPopup ("##workflowMenu")) {
        selected = WorkflowMenu (spec.tabs, held);
        ImGui::EndPopup ();
    }
    ImGui::SameLine ();
    collapse = ImGui::SmallButton ("-##collapse");
    Tip ("Collapse panel; keep drafts and selection");
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
    if (current && Global (held)) {
        ImGui::TextUnformatted (current->title.c_str ());
        if (workflow && ImGui::SmallButton (("Back to " + workflow->title).c_str ()))
            selected = workflow->key;
    }
    return selected;
}

std::string NavigationInput (const HostSpec& spec, NavigationState& state, bool& active)
{
    std::string selected;
    ImGui::Separator ();
    ImGui::SetNextItemWidth (-1);
    ImGui::BeginDisabled (!spec.textInput);
    const char* hint =
        spec.textInput ? "show Settings / workflow <name>" : "Keyboard commands unavailable; use selector";
    const bool submit =
        ImGui::InputTextWithHint ("##hudCommand", hint, state.input, sizeof state.input,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll);
    active = ImGui::IsItemActive ();
    ImGui::EndDisabled ();
    Tip (spec.textInput ? "Navigation commands only in this pass. Use the exact page name."
                        : "Keyboard input is not available in this viewer; use the workflow selector.");
    if (submit) {
        selected = ResolveNavigation (state.input, spec.tabs);
        state.message = selected.empty () ? "No unique page matches. Use its exact name from the selector." : "";
        if (!selected.empty ())
            state.input[0] = '\0';
    }
    ErrorNotice (state.message, spec.ui);
    return selected;
}
} // namespace geomsrv::archviz::hudshell
