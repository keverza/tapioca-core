#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include <imgui.h>
#include <cstring>
#include <string>

// #191 area D, the Outliner: under the Inspector. Three views of one document -- its objects,
// layers and groups; the landscape's compiled terrain recipe; its issues -- in a block of its own
// scroll, about twelve rows high, that collapses to one line. Rows and the view's selection follow
// each other both ways, and the row under the pointer is what the view highlights.
namespace geomsrv::archviz::edithud {
using namespace detail;

namespace {
constexpr int kRows = 12;

// A view's body in a child of its own scroll, capped so the Inspector above keeps its room.
template <typename Draw> void ViewBody (const char* id, Draw&& draw)
{
    const float height = ImGui::GetFrameHeightWithSpacing () * float (kRows);
    if (ImGui::BeginChild (id, ImVec2 (0.0f, height), ImGuiChildFlags_Borders))
        draw ();
    ImGui::EndChild ();
}

bool ShowTab (Session& s, int view)
{
    return s.outlinerShow == view;
}
} // namespace

size_t detail::IssueCount (const Session& s)
{
    if (!s.edits.Doc ().landscape)
        return 0;
    return s.landscape.diagnostics.size () + s.landscape.planarDiagnostics.size () + !s.landscape.error.empty ();
}

void detail::Outliner (Session& s)
{
    const auto& doc = Doc (s);
    s.outlinerHover = {};
    // The view selected something the Outliner has not shown yet: bring its row into sight once.
    const bool reveal = s.selection.Items () != s.outlinerSeen;
    s.outlinerSeen = s.selection.Items ();
    if (s.outlinerShow >= 0)
        s.outlinerCollapsed = false;
    ImGui::Separator ();
    ImGui::PushID ("outliner");
    if (ImGui::ArrowButton ("##collapse", s.outlinerCollapsed ? ImGuiDir_Right : ImGuiDir_Down))
        s.outlinerCollapsed = !s.outlinerCollapsed;
    if (ImGui::IsItemHovered ())
        hudshell::HoverTip (s.outlinerCollapsed ? "Show the Outliner" : "Collapse the Outliner to this line");
    ImGui::SameLine ();
    ImGui::AlignTextToFramePadding ();
    // Every row the Objects view lists, paths among them, and how many of those are paths.
    const size_t objects = doc.entities.size () + doc.paths.size ();
    ImGui::Text ("%zu object%s (%zu path%s)   %zu selected", objects, objects == 1 ? "" : "s", doc.paths.size (),
                 doc.paths.size () == 1 ? "" : "s", s.selection.Items ().size ());
    if (s.outlinerCollapsed) {
        ImGui::PopID ();
        return;
    }
    // Search: filters the view it is typed over.
    char filter[96] = {};
    std::strncpy (filter, s.outlinerFilter.c_str (), sizeof filter - 1);
    const float select = ImGui::CalcTextSize ("Select").x + 2.0f * ImGui::GetStyle ().FramePadding.x;
    ImGui::SetNextItemWidth (ImGui::GetContentRegionAvail ().x - select - ImGui::GetStyle ().ItemSpacing.x);
    if (ImGui::InputTextWithHint ("##search", "Search", filter, sizeof filter))
        s.outlinerFilter = filter;
    ImGui::SameLine ();
    if (ImGui::Button ("Select"))
        ImGui::OpenPopup ("##select");
    if (ImGui::IsItemHovered ())
        hudshell::HoverTip ("Select all, invert, or select what shares the selection's role, layer or kind.");
    if (ImGui::BeginPopup ("##select")) {
        SelectMenuItems (s);
        ImGui::EndPopup ();
    }
    if (hudshell::BeginSubTabs ("##views")) {
        if (ImGui::BeginTabItem ("Objects", nullptr, ShowTab (s, 0) ? ImGuiTabItemFlags_SetSelected : 0)) {
            ViewBody ("##objects", [&] {
                OrganizeVisibility (s);
                // The user, 2026-11-20: one tree, the layers as folders of their objects, and its
                // header collapses all of it. A search lists what matches, flat.
                if (!s.outlinerFilter.empty ())
                    ObjectRows (s, s.outlinerFilter, reveal);
                else if (ImGui::CollapsingHeader ("Layers", ImGuiTreeNodeFlags_DefaultOpen))
                    LayerRows (s, reveal);
            });
            ImGui::EndTabItem ();
        }
        if (doc.landscape) {
            ImGui::BeginDisabled (s.landscapeCommand.has_value ());
            if (ImGui::BeginTabItem ("Recipe", nullptr, ShowTab (s, 1) ? ImGuiTabItemFlags_SetSelected : 0)) {
                ViewBody ("##recipe", [&] { OperationsTab (s); });
                ImGui::EndTabItem ();
            }
            // The count rides in the label; ### keeps the tab's identity as it changes.
            const auto issues = IssueCount (s);
            const std::string label = issues ? "Issues " + std::to_string (issues) + "###issues" : "Issues###issues";
            if (ImGui::BeginTabItem (label.c_str (), nullptr, ShowTab (s, 2) ? ImGuiTabItemFlags_SetSelected : 0)) {
                ViewBody ("##issues", [&] { WarningsTab (s); });
                ImGui::EndTabItem ();
            }
            ImGui::EndDisabled ();
        }
        hudshell::EndSubTabs ();
    }
    s.outlinerShow = -1;
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::edithud
