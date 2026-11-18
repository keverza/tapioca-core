#include "ArchViz/SurfaceTakeoffPage.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SurfaceTakeoffLayer.hpp"
#include "Geometry/SurfaceTakeoffController.hpp"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string_view>

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

std::string MaterialLabel (const geomsrv::surfacetakeoff::SurfaceRow& row)
{
    return row.name + " [" + std::to_string (row.surfaceAttributeIndex) + "]";
}

bool Swatch (uint32_t rgba)
{
    const auto color = ImVec4 (float ((rgba >> 24) & 255u) / 255.0f, float ((rgba >> 16) & 255u) / 255.0f,
                               float ((rgba >> 8) & 255u) / 255.0f, 1.0f);
    const float size = ImGui::GetTextLineHeight ();
    const bool pressed = ImGui::ColorButton (
        "##sourceColor", color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2 (size, size));
    ImGui::SameLine ();
    return pressed;
}

std::optional<PageAction> MaterialPicker (PageState& page, const Result& result, const HighlightSummary& highlight)
{
    if (!ImGui::CollapsingHeader ("Filter by material"))
        return std::nullopt;
    ImGui::SetNextItemWidth (-1);
    ImGui::InputTextWithHint ("##materialSearch", "Search measured materials...", page.materialSearch.data (),
                              page.materialSearch.size ());
    page.textActive = ImGui::IsItemActive ();
    const std::string_view needle (page.materialSearch.data ());
    const auto same = [] (unsigned char a, unsigned char b) {
        const auto fold = [] (unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        return fold (a) == fold (b);
    };
    if (ImGui::BeginListBox ("##measuredMaterials", ImVec2 (-1, ImGui::GetTextLineHeightWithSpacing () * 5))) {
        bool found = false;
        for (const auto& row : result.surfaces) {
            const auto label = MaterialLabel (row);
            if (row.areaM2 <= 0 ||
                std::search (label.begin (), label.end (), needle.begin (), needle.end (), same) == label.end ())
                continue;
            found = true;
            ImGui::PushID (row.surfaceAttributeIndex);
            bool checked = page.draftMaterials.contains (row.surfaceAttributeIndex);
            if (ImGui::Checkbox (label.c_str (), &checked)) {
                if (checked)
                    page.draftMaterials.insert (row.surfaceAttributeIndex);
                else
                    page.draftMaterials.erase (row.surfaceAttributeIndex);
            }
            ImGui::PopID ();
        }
        if (!found)
            ImGui::TextDisabled ("No matching measured materials");
        ImGui::EndListBox ();
    }
    ImGui::TextDisabled ("An empty selection includes all measured materials.");
    const bool changed = page.draftMaterials != highlight.materials;
    ImGui::BeginDisabled (!changed);
    const bool apply = ImGui::Button ("Apply materials");
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    ImGui::BeginDisabled (page.draftMaterials.empty ());
    if (ImGui::Button ("Clear selection"))
        page.draftMaterials.clear ();
    ImGui::EndDisabled ();
    if (!apply)
        return std::nullopt;
    page.page = 0;
    return PageAction { PageAction::Kind::FilterMaterials, page.scope, highlight.generation, {}, page.draftMaterials };
}

std::optional<PageAction> MeasuredRows (PageState& page, const Result& result, const HighlightSummary& highlight)
{
    std::optional<PageAction> action;
    const bool canIsolate = highlight.generation != 0 && highlight.generation == page.shownGeneration;
    // The scope combo is a draft: until Refresh, the table is still the previous scope's.
    if (result.classification.rfind ("all", 0) == 0 && result.classification.rfind (RegionMode (page.scope), 0) != 0)
        ImGui::TextWrapped ("These areas are for the previous target scope. Press Refresh to calculate %s.",
                            page.scope == TargetScope::Walls ? "captured walls" : "captured construction");
    if (result.classification == "allElementFaces-contactOnly")
        ImGui::TextWrapped ("Captured construction faces, including interiors.");
    else if (result.classification == "allWallFaces-contactOnly")
        ImGui::TextWrapped ("Captured wall faces, including interiors.");
    else
        ImGui::TextWrapped ("User-confirmed exterior facade faces.");
    ImGui::TextUnformatted ("Calculated area before filters (m²)");
    Area (result.selectedAreaM2);
    if (canIsolate) {
        ImGui::TextWrapped ("%s %.3f m² (%zu triangles)", highlight.material ? "Isolated" : "Highlighted",
                            highlight.areaM2, highlight.triangles);
        if (highlight.material && ImGui::Button ("Show all surfaces"))
            action = PageAction { PageAction::Kind::Isolate, page.scope, highlight.generation };
        if (highlight.Focused ()) {
            const bool white = highlight.contextStyle == ContextStyle::WhiteModel;
            int style = white ? 1 : 0;
            const char* styles[] = { "Desaturated model", "White model" };
            ImGui::TextUnformatted ("Rest of the model");
            ImGui::SetNextItemWidth (-1);
            if (ImGui::Combo ("##contextStyle", &style, styles, 2)) {
                PageAction changed { PageAction::Kind::ContextStyle, page.scope, highlight.generation };
                changed.contextStyle = style == 1 ? ContextStyle::WhiteModel : ContextStyle::Desaturated;
                action = std::move (changed);
            }
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip (white ? "The model renders white. The selected surface keeps its own colour, shows "
                                           "through the model at 30%% and its hidden contours are dashed."
                                         : "The model keeps its shading without colour. The selected surface keeps "
                                           "its own colour.");
            int opacity = int (std::lround (highlight.contextOpacity * 100));
            ImGui::SetNextItemWidth (-1);
            if (ImGui::SliderInt ("##contextOpacity", &opacity, 0, 100, "Context opacity %d%%")) {
                PageAction changed { PageAction::Kind::ContextOpacity, page.scope, highlight.generation };
                changed.contextOpacity = opacity / 100.0;
                action = std::move (changed);
            }
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip (white ? "Fade the white model toward a white backdrop. The selected highlight "
                                           "stays unchanged."
                                         : "Fade the desaturated model toward a neutral backdrop. The selected "
                                           "highlight stays unchanged.");
        }
    }
    if (result.selectedAreaM2 <= 0)
        return action;

    if (canIsolate) {
        if (!highlight.materials.empty ())
            ImGui::Text ("%zu materials selected", highlight.materials.size ());
        if (auto filterAction = MaterialPicker (page, result, highlight))
            action = std::move (filterAction);
    }
    std::vector<const geomsrv::surfacetakeoff::SurfaceRow*> rows;
    for (const auto& row : result.surfaces) {
        const auto area = highlight.rowAreas.find (row.surfaceAttributeIndex);
        if (canIsolate ? area != highlight.rowAreas.end () && area->second > 0 : row.areaM2 > 0)
            rows.push_back (&row);
    }
    const size_t pageCount = (rows.size () + kRowsPerPage - 1) / kRowsPerPage;
    if (pageCount == 0) {
        ImGui::TextWrapped ("No measured surfaces match the applied filters.");
        return action;
    }
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
            const auto label = MaterialLabel (row);
            ImGui::PushID (row.surfaceAttributeIndex);
            ImGui::BeginDisabled (!canIsolate);
            const bool isolated = canIsolate && highlight.material == row.surfaceAttributeIndex;
            const bool swatch = Swatch (row.rgba);
            if (ImGui::Selectable (label.c_str (), isolated, ImGuiSelectableFlags_SpanAllColumns) || swatch) {
                action = PageAction { PageAction::Kind::Isolate, page.scope, highlight.generation };
                if (!isolated)
                    action->material = row.surfaceAttributeIndex;
            }
            ImGui::EndDisabled ();
            ImGui::PopID ();
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip ("Captured Archicad surface attribute. Names can repeat across attribute IDs.");
            ImGui::TableSetColumnIndex (1);
            Area (canIsolate ? highlight.rowAreas.at (row.surfaceAttributeIndex) : row.areaM2);
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
    return action;
}
// The image of what is highlighted, with its area table under it.
std::optional<PageAction> ShotRow (PageState& page, const HighlightSummary& highlight, ShotControls* shot)
{
    ImGui::SeparatorText ("Screenshot");
    if (shot == nullptr) {
        ImGui::TextDisabled ("Screenshot available in the standalone viewer.");
        return std::nullopt;
    }
    std::optional<PageAction> action;
    std::vector<const char*> labels;
    for (const auto& preset : shot->presets)
        labels.push_back (preset.c_str ());
    ImGui::BeginDisabled (shot->busy || labels.empty ());
    if (!labels.empty ()) {
        shot->preset = std::clamp (shot->preset, 0, int (labels.size ()) - 1);
        ImGui::SetNextItemWidth (ImGui::GetContentRegionAvail ().x * 0.55f);
        ImGui::Combo ("##shotScale", &shot->preset, labels.data (), int (labels.size ()));
        ImGui::SameLine ();
    }
    if (ImGui::Button ("Screenshot", ImVec2 (-1.0f, 0.0f)))
        action = PageAction { PageAction::Kind::Screenshot, page.scope, highlight.generation };
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip ("Renders the view at the chosen size without panels or gizmo, adds the area table of "
                           "what is highlighted below it and saves a PNG to Tapioca\\Export.");
    if (shot->busy)
        ImGui::TextDisabled ("Rendering screenshot...");
    else if (shot->failed)
        ImGui::TextWrapped ("Screenshot not saved: %s", shot->result.c_str ());
    else if (!shot->result.empty ())
        ImGui::TextWrapped ("Saved %s", shot->result.c_str ());
    return action;
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
                                    const std::string& actionError, const HighlightSummary& highlight,
                                    ShotControls* shot)
{
    page.textActive = false;
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
            page.materialSearch = {};
            page.draftMaterials = highlight.materials;
        }
        if (auto rowAction =
                MeasuredRows (page, *std::static_pointer_cast<const Result> (view->lastGood->payload), highlight))
            action = *rowAction;
        if (highlight.generation == view->generation && highlight.areaM2 > 0)
            if (auto shotAction = ShotRow (page, highlight, shot))
                action = *shotAction;
    }
    ImGui::PopID ();
    return action;
}

} // namespace geomsrv::archviz::surfacetakeoff
