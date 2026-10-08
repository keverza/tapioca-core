#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::buildingplan {
std::vector<hudmeta::Edit> Draw (const Plan& plan, Draft& draft, float scale,
                                 const massingareas::Coefficients& coefficients)
{
    std::vector<hudmeta::Edit> edits;
    if (!ImGui::CollapsingHeader ("Plan view", ImGuiTreeNodeFlags_DefaultOpen)) {
        draft.placing = false;
        return edits;
    }
    if (!plan.note.empty ())
        ImGui::TextWrapped ("%s", plan.note.c_str ());
    const auto* floor = Displayed (plan, draft);
    if (!floor) {
        draft.placing = false;
        ImGui::TextDisabled ("No complete current floor contour.");
        return edits;
    }
    // A successful deferred Save is acknowledged by the next published metadata
    // snapshot. Other external changes never silently overwrite a local draft.
    if (!draft.known || (!Dirty (draft) && Conflict (plan, draft)) ||
        (Dirty (draft) && !plan.mixed && plan.saved == draft.points && plan.guids == draft.guids))
        Reset (plan, draft);
    const bool conflict = Conflict (plan, draft);
    ImGui::Text ("Floor %d | Z %.2f m | gross %.2f m2", floor->story, floor->z,
                 floor->areaM2 * coefficients.grossFactor);
    ImGui::TextDisabled ("Click a floor in the section to change this plan. +Y is up.");

    ImGui::BeginDisabled (conflict || draft.points.size () >= kMaxStairs);
    if (ImGui::Button ("Add stairwell")) {
        draft.selected = -1;
        draft.placing = true;
    }
    ImGui::EndDisabled ();
    if (draft.selected >= 0 && size_t (draft.selected) < draft.points.size ()) {
        ImGui::SameLine ();
        ImGui::BeginDisabled (conflict);
        if (ImGui::Button ("Move"))
            draft.placing = true;
        ImGui::SameLine ();
        if (ImGui::Button ("Remove")) {
            draft.points.erase (draft.points.begin () + draft.selected);
            draft.selected = -1;
            draft.placing = false;
            draft.changed = true;
        }
        ImGui::EndDisabled ();
    }
    if (draft.placing) {
        ImGui::TextWrapped ("Click inside the blue contour to %s a point (not in a courtyard).",
                            draft.selected < 0 ? "add" : "move");
        if (ImGui::SmallButton ("Cancel placement"))
            draft.placing = false;
    }
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const auto* groups : { &floor->physical, &floor->contours })
        for (const auto& contours : *groups)
            for (const auto& chain : contours)
                for (size_t i = 0; i < chain.Count (); ++i) {
                    minX = (std::min) (minX, chain.xy[i * 2]);
                    maxX = (std::max) (maxX, chain.xy[i * 2]);
                    minY = (std::min) (minY, chain.xy[i * 2 + 1]);
                    maxY = (std::max) (maxY, chain.xy[i * 2 + 1]);
                }
    if (minX <= maxX && minY <= maxY) {
        const auto origin = ImGui::GetCursorScreenPos ();
        const ImVec2 extent ((std::max) (80.0f, ImGui::GetContentRegionAvail ().x),
                             float (graphicssettings::Number ("diagram.height", 200)) * scale);
        const float margin = (std::min) ({ 24.0f * scale, extent.x / 4, extent.y / 4 });
        const double factor = (std::min) ((extent.x - 2 * margin) / (std::max) (maxX - minX, 1e-9),
                                          (extent.y - 2 * margin) / (std::max) (maxY - minY, 1e-9));
        const double centerX = minX + (maxX - minX) / 2, centerY = minY + (maxY - minY) / 2;
        const auto project = [&] (Point point) {
            return ImVec2 (origin.x + extent.x / 2 + float ((point.x - centerX) * factor),
                           origin.y + extent.y / 2 - float ((point.y - centerY) * factor));
        };
        const auto unproject = [&] (ImVec2 point) {
            return Point { centerX + (point.x - origin.x - extent.x / 2) / factor,
                           centerY - (point.y - origin.y - extent.y / 2) / factor };
        };
        ImGui::InvisibleButton ("##buildingPlan", extent);
        const bool hovered = ImGui::IsItemHovered ();
        auto* draw = ImGui::GetWindowDrawList ();
        const ImVec2 end (origin.x + extent.x, origin.y + extent.y);
        draw->AddRectFilled (origin, end, ImGui::GetColorU32 (ImGuiCol_FrameBg), 4 * scale);
        draw->PushClipRect (origin, end, true);
        const auto outlines = [&] (const auto& groups, ImU32 colour, float width) {
            for (const auto& contours : groups)
                for (const auto& chain : contours) {
                    std::vector<ImVec2> points;
                    for (size_t i = 0; i < chain.Count (); ++i)
                        points.push_back (project ({ chain.xy[i * 2], chain.xy[i * 2 + 1] }));
                    draw->AddPolyline (points.data (), int (points.size ()), colour, ImDrawFlags_Closed, width);
                }
        };
        outlines (floor->physical, IM_COL32 (140, 140, 140, 150), scale);
        outlines (floor->contours, IM_COL32 (74, 144, 217, 255), 2 * scale);
        int near = -1;
        float best = 12 * scale;
        const auto mouse = ImGui::GetIO ().MousePos;
        for (size_t i = 0; i < draft.points.size (); ++i) {
            const auto at = project (draft.points[i]);
            const float distance = std::hypot (at.x - mouse.x, at.y - mouse.y);
            if (distance < best) {
                best = distance;
                near = int (i);
            }
            const ImU32 colour =
                Contains (*floor, draft.points[i]) ? IM_COL32 (255, 186, 0, 255) : IM_COL32 (229, 72, 77, 255);
            draw->AddCircle (at, (draft.selected == int (i) ? 8 : 6) * scale, colour, 0, 2 * scale);
            draw->AddLine ({ at.x - 4 * scale, at.y }, { at.x + 4 * scale, at.y }, colour, scale);
            draw->AddLine ({ at.x, at.y - 4 * scale }, { at.x, at.y + 4 * scale }, colour, scale);
            draw->AddText ({ at.x + 9 * scale, at.y }, colour, std::to_string (i + 1).c_str ());
        }
        if (hovered && !conflict) {
            if (draft.placing) {
                ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked (ImGuiMouseButton_Left))
                    Place (*floor, draft, unproject (mouse));
            }
            else if (ImGui::IsMouseClicked (ImGuiMouseButton_Left))
                draft.selected = near;
        }
        draw->PopClipRect ();
        ImGui::TextDisabled ("Blue: counted floor | Gray: physical outline | XY: project metres");
    }
    for (size_t i = 0; i < draft.points.size (); ++i) {
        ImGui::PushID (int (i));
        const std::string label = "Stair " + std::to_string (i + 1) + ": X " + hudmeta::NumberText (draft.points[i].x) +
                                  ", Y " + hudmeta::NumberText (draft.points[i].y);
        if (ImGui::Selectable (label.c_str (), draft.selected == int (i))) {
            draft.selected = int (i);
            draft.placing = false;
        }
        if (!Contains (*floor, draft.points[i]))
            ImGui::TextWrapped ("Outside this floor's counted contour; relocate if required. Shared point retained.");
        ImGui::PopID ();
    }
    const bool large = NeedsTwoStairs (floor->areaM2, coefficients);
    const bool buildingLarge = std::any_of (plan.floors.begin (), plan.floors.end (), [&] (const auto& item) {
        return NeedsTwoStairs (item.areaM2, coefficients);
    });
    if (large)
        ImGui::TextWrapped ("Gross floor area is larger than 500 m2: this zone requires at least two stairs.");
    else if (buildingLarge)
        ImGui::TextWrapped ("Another floor is larger than 500 m2 gross: this building requires at least two stairs.");
    if (buildingLarge) {
        const size_t inside = size_t (std::count_if (draft.points.begin (), draft.points.end (),
                                                     [&] (Point point) { return Contains (*floor, point); }));
        ImGui::TextWrapped ("Proposed building-wide: %zu stairs (%zu inside this floor). Approximate locations only; "
                            "no egress validation.",
                            draft.points.size (), inside);
    }
    if (plan.mixed)
        ImGui::TextWrapped ("Members have different or invalid saved stairwell metadata. Add points and Save to "
                            "replace it for the whole building.");
    if (conflict)
        ImGui::TextWrapped ("Building membership or saved locations changed. Discard to reload before saving.");
    if (plan.mixed || !draft.points.empty ()) {
        ImGui::BeginDisabled (conflict);
        if (ImGui::SmallButton ("Clear all proposed stairs")) {
            draft.points.clear ();
            draft.changed = true;
            draft.selected = -1;
            draft.placing = false;
        }
        ImGui::EndDisabled ();
    }
    ImGui::BeginDisabled (!Dirty (draft) || conflict);
    if (ImGui::Button ("Save stairwells")) {
        edits = Edits (plan, draft);
        draft.placing = false;
    }
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::Button ("Discard"))
        Reset (plan, draft);
    ImGui::TextDisabled ("%s | Building-wide metadata only; floor generation comes later.",
                         Dirty (draft) ? "Local changes not yet saved" : "No local changes");
    return edits;
}
} // namespace geomsrv::archviz::buildingplan
