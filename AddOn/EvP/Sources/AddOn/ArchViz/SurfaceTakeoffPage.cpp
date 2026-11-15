#include "ArchViz/SurfaceTakeoffPage.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SurfaceTakeoffLayer.hpp"
#include "Geometry/SurfaceTakeoffController.hpp"

#include <imgui.h>
#include <algorithm>

namespace geomsrv::archviz::surfacetakeoff {
namespace {
using geomsrv::surfacetakeoff::ControllerState;
using geomsrv::surfacetakeoff::Result;
constexpr size_t kRowsPerPage = 25;

const char* Status (ControllerState state)
{
    switch (state) {
        case ControllerState::Empty:
            return "No calculation yet.";
        case ControllerState::Pending:
            return "Calculation in progress.";
        case ControllerState::Current:
            return "Complete for the captured scope.";
        case ControllerState::Stale:
            return "The previous result is stale. Refresh to update it.";
        case ControllerState::Refused:
            return "Calculation refused.";
        case ControllerState::Cancelled:
            return "Calculation cancelled.";
    }
    return "";
}

void Area (double area)
{
    if (area > 0 && area < 0.0005)
        ImGui::TextUnformatted ("<0.001");
    else
        ImGui::Text ("%.3f", area);
}

void MeasuredRows (PageState& page, const Result& result)
{
    if (result.classification == "allElementFaces-contactOnly")
        ImGui::TextWrapped ("Captured construction faces, including interiors.");
    else if (result.classification == "allWallFaces-contactOnly")
        ImGui::TextWrapped ("Captured wall faces, including interiors.");
    else
        ImGui::TextWrapped ("User-confirmed exterior facade faces.");
    ImGui::TextUnformatted ("Calculated area before filters (m²)");
    Area (result.selectedAreaM2);
    if (result.selectedAreaM2 <= 0)
        return;

    std::vector<const geomsrv::surfacetakeoff::SurfaceRow*> rows;
    for (const auto& row : result.surfaces)
        if (row.areaM2 > 0)
            rows.push_back (&row);
    const size_t pageCount = (rows.size () + kRowsPerPage - 1) / kRowsPerPage;
    if (pageCount == 0)
        return;
    page.page = (std::min) (page.page, pageCount - 1);
    if (ImGui::BeginTable ("surfaceAreas", 2,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                               ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn ("Surface", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("m²", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow ();
        const size_t begin = page.page * kRowsPerPage;
        const size_t end = (std::min) (begin + kRowsPerPage, rows.size ());
        for (size_t i = begin; i < end; ++i) {
            const auto& row = *rows[i];
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            ImGui::TextWrapped ("%s [%d]", row.name.c_str (), row.surfaceAttributeIndex);
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip ("Captured Archicad surface attribute. Names can repeat across attribute IDs.");
            ImGui::TableSetColumnIndex (1);
            Area (row.areaM2);
        }
        ImGui::EndTable ();
    }
    if (pageCount > 1) {
        ImGui::BeginDisabled (page.page == 0);
        if (ImGui::Button ("Previous"))
            --page.page;
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        ImGui::BeginDisabled (page.page + 1 >= pageCount);
        if (ImGui::Button ("Next"))
            ++page.page;
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        ImGui::Text ("%zu / %zu", page.page + 1, pageCount);
    }
}
} // namespace

hudshell::HostTab WorkflowTab ()
{
    hudshell::HostTab tab { kWorkflowKey, "Exposed surfaces" };
    tab.family = hudshell::WorkflowFamily::Document;
    tab.token = "exposed surfaces";
    tab.purpose = "Calculate exposed surface areas by material.";
    return tab;
}

const char* RegionMode (TargetScope scope)
{
    return scope == TargetScope::Walls ? "allWallFaces" : "allElementFaces";
}

const char* LauncherScope (TargetScope scope)
{
    return scope == TargetScope::Walls ? "All walls" : "Criteria - captured elements";
}

std::optional<PageAction> DrawPage (PageState& page, const geomsrv::surfacetakeoff::ControllerView* view,
                                    const std::string& actionError)
{
    if (view == nullptr) {
        ImGui::TextDisabled ("Updating calculation status…");
        return std::nullopt;
    }
    ImGui::PushID ("exposedSurfaces");
    ImGui::TextUnformatted ("Measure: Exposed");
    ImGui::TextUnformatted ("Target scope");
    const bool pending = view->state == ControllerState::Pending;
    ImGui::BeginDisabled (pending);
    ImGui::SetNextItemWidth (-1);
    const char* choices[] = { "Captured construction", "Captured walls" };
    int selected = page.scope == TargetScope::Walls ? 1 : 0;
    if (ImGui::Combo ("##targetScope", &selected, choices, 2))
        page.scope = selected == 1 ? TargetScope::Walls : TargetScope::Construction;
    ImGui::EndDisabled ();

    std::optional<PageAction> action;
    if (pending) {
        if (ImGui::Button ("Cancel"))
            action = PageAction { PageAction::Kind::Cancel, page.scope, view->generation };
    }
    else if (ImGui::Button (view->lastGood ? "Refresh" : "Calculate"))
        action = PageAction { PageAction::Kind::Calculate, page.scope, view->generation };

    ImGui::TextWrapped ("%s", Status (view->state));
    if (!actionError.empty ())
        ImGui::TextWrapped ("%s", actionError.c_str ());
    if (!view->error.empty ())
        ImGui::TextWrapped ("%s", view->error.c_str ());
    if (view->state == ControllerState::Current && view->lastGood && view->lastGood->generation == view->generation &&
        view->lastGood->payload) {
        if (page.shownGeneration != view->generation) {
            page.shownGeneration = view->generation;
            page.page = 0;
        }
        MeasuredRows (page, *std::static_pointer_cast<const Result> (view->lastGood->payload));
    }
    ImGui::PopID ();
    return action;
}

} // namespace geomsrv::archviz::surfacetakeoff
