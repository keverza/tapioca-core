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
    const auto owner = reinterpret_cast<uintptr_t> (ImGui::GetCurrentContext ());
    if (!ImGui::CollapsingHeader ("Plan view", ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool otherGesture =
            std::any_of (draft.quickPlans.begin (), draft.quickPlans.end (),
                         [&] (const auto& item) { return item.second.dragging && item.second.owner != owner; });
        if ((!draft.dragging || draft.dragOwner == owner) && !otherGesture)
            Cancel (draft);
        return edits;
    }
    if (!plan.note.empty ())
        ImGui::TextWrapped ("%s", plan.note.c_str ());
    const auto* floor = Displayed (plan, draft);
    if (!floor) {
        Cancel (draft);
        ImGui::TextDisabled ("No complete current floor contour.");
        return edits;
    }
    // A successful deferred Save is acknowledged by the next published metadata
    // snapshot. Other external changes never silently overwrite a local draft.
    Sync (plan, draft);
    const bool conflict = Conflict (plan, draft);
    if (conflict || ImGui::IsKeyPressed (ImGuiKey_Escape))
        Cancel (draft);
    ImGui::Text ("Floor %d | Z %.2f m | gross %.2f m2", floor->story, floor->z,
                 floor->areaM2 * coefficients.grossFactor);
    ImGui::TextDisabled ("4.5 x 4.1 m | Project XY, +Y up | Snap within 0.5 m");

    const bool active = draft.placing || draft.moving;
    ImGui::BeginDisabled (!active && (conflict || draft.points.size () >= kMaxStairs));
    if (active) {
        ImGui::PushStyleColor (ImGuiCol_Button, ImVec4 (0.78f, 0.34f, 0.07f, 1));
        ImGui::PushStyleColor (ImGuiCol_ButtonHovered, ImVec4 (0.95f, 0.46f, 0.10f, 1));
        ImGui::PushStyleColor (ImGuiCol_ButtonActive, ImVec4 (0.65f, 0.27f, 0.05f, 1));
    }
    const float buttonWidth = ImGui::CalcTextSize ("Cancel placement").x + 2 * ImGui::GetStyle ().FramePadding.x;
    ImGui::PushID ("stairPlacement");
    if (ImGui::Button (active ? "Cancel placement" : "Add stairwell", { buttonWidth, 0 })) {
        if (active)
            Cancel (draft);
        else {
            draft.selected = -1;
            draft.placing = true;
        }
    }
    ImGui::PopID ();
    if (active)
        ImGui::PopStyleColor (3);
    ImGui::EndDisabled ();
    if (draft.selected >= 0 && size_t (draft.selected) < draft.points.size ()) {
        ImGui::SameLine ();
        ImGui::BeginDisabled (conflict);
        if (ImGui::Button ("Move")) {
            Cancel (draft);
            draft.moving = true;
        }
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Press and drag the selected rectangle. Escape or Cancel restores an unfinished drag.");
        ImGui::SameLine ();
        if (ImGui::Button ("Remove")) {
            Cancel (draft);
            draft.points.erase (draft.points.begin () + draft.selected);
            draft.selected = -1;
            draft.placing = false;
            draft.changed = true;
        }
        ImGui::EndDisabled ();
    }
    auto& quick = QuickFor (plan, draft, *floor);
    ImGui::SetNextItemWidth (150 * scale);
    if (ImGui::Combo ("##quickStage", &quick.stage,
                      "S0 Input\0S1 Local frame\0S2 Bars\0S3 Circulation\0S4 Bands\0S5 Segments\0Quick apartments\0"))
        CancelUnits (quick);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Reset plan")) {
        CancelUnits (quick);
        ResetQuick (plan, draft, *floor);
    }
    if (!draft.uniqueFloors.contains (floor->story)) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Make unique")) {
            CancelUnits (quick);
            MakeUnique (plan, draft, *floor);
        }
    }
    // Reacquire: Reset/Make unique may replace the map entry behind the old reference.
    auto& design = QuickFor (plan, draft, *floor);
    if (design.stage == 6 && design.ready) {
        ImGui::BeginDisabled (conflict || draft.placing || draft.moving || design.dragging);
        if (ImGui::SmallButton (design.adding ? "Cancel unit" : "Add unit"))
            design.adding = !design.adding;
        ImGui::SameLine ();
        ImGui::BeginDisabled (design.selected < 0);
        if (ImGui::SmallButton ("Delete unit"))
            RemoveUnit (design, size_t (design.selected));
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        const double rooms = design.selected >= 0 ? design.seeds[size_t (design.selected)].rooms : design.newRooms;
        int roomType = rooms == 1 ? 0 : rooms == 1.5 ? 1 : rooms == 2 ? 2 : rooms == 3 ? 3 : 4;
        ImGui::SetNextItemWidth (100 * scale);
        if (ImGui::Combo ("##unitRooms", &roomType,
                          "1 room\0"
                          "1.5 rooms\0"
                          "2 rooms\0"
                          "3 rooms\0"
                          "4 rooms\0")) {
            const double values[] = { 1, 1.5, 2, 3, 4 };
            design.newRooms = values[roomType];
            if (design.selected >= 0)
                SetUnitRooms (design, size_t (design.selected), design.newRooms);
        }
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip (
                "Area weight for the selected unit (or the next added unit). Not internal room generation.");
        ImGui::EndDisabled ();
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
        const auto regions = [&] (const auto& values, ImU32 colour) {
            for (const auto& region : values)
                outlines (std::vector<std::vector<SliceChain>> { region.rings }, colour, 1.5f * scale);
        };
        if (design.stage == 1 && !design.bars.empty ()) {
            const auto o = project (design.origin);
            draw->AddLine (o,
                           project ({ design.origin.x + 5 * std::cos (design.angle),
                                      design.origin.y + 5 * std::sin (design.angle) }),
                           IM_COL32 (229, 72, 77, 255), 2 * scale);
            draw->AddLine (o,
                           project ({ design.origin.x - 5 * std::sin (design.angle),
                                      design.origin.y + 5 * std::cos (design.angle) }),
                           IM_COL32 (127, 200, 169, 255), 2 * scale);
        }
        if (design.stage == 2)
            regions (design.bars, IM_COL32 (176, 136, 201, 255));
        if (design.stage >= 3)
            regions (design.corridors, IM_COL32 (214, 196, 154, 255));
        if (design.stage == 4)
            regions (design.bands, IM_COL32 (154, 209, 230, 255));
        if (design.stage == 5)
            regions (design.segments, IM_COL32 (74, 127, 181, 255));
        if (design.stage == 6)
            for (size_t i = 0; i < design.units.size (); ++i)
                outlines (std::vector<std::vector<SliceChain>> { design.units[i].rings },
                          hudshell::Packed (UnitColour (design.seeds[i].rooms)), 1.5f * scale);
        int near = -1;
        double best = 1e300;
        const auto mouse = ImGui::GetIO ().MousePos;
        const auto worldMouse = unproject (mouse);
        for (size_t i = 0; i < draft.points.size (); ++i) {
            const auto point = draft.points[i];
            const double distance = std::hypot (point.x - worldMouse.x, point.y - worldMouse.y);
            if (std::abs (point.x - worldMouse.x) <= kStairWidth / 2 &&
                std::abs (point.y - worldMouse.y) <= kStairDepth / 2 && distance < best) {
                best = distance;
                near = int (i);
            }
        }
        int nearUnit = -1;
        float unitDistance = 10 * scale;
        if (design.stage == 6 && design.ready)
            for (size_t i = 0; i < design.seeds.size (); ++i) {
                const auto center = project (UnitCenter (design, design.seeds[i]));
                const float distance = std::hypot (center.x - mouse.x, center.y - mouse.y);
                if (distance < unitDistance) {
                    unitDistance = distance;
                    nearUnit = int (i);
                }
            }
        if (hovered && !conflict && ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
            if (draft.placing)
                Place (*floor, draft, worldMouse);
            else if (draft.moving && near == draft.selected)
                BeginDrag (draft, worldMouse, owner);
            else if (design.stage == 6 && design.adding)
                AddUnit (design, worldMouse);
            else if (design.stage == 6 && nearUnit >= 0 && near < 0) {
                Cancel (draft);
                design.selected = nearUnit;
                design.dragOriginal = design.seeds[size_t (nearUnit)].along;
                const auto center = UnitCenter (design, design.seeds[size_t (nearUnit)]);
                design.dragOffset = { center.x - worldMouse.x, center.y - worldMouse.y };
                design.dragging = true;
                design.owner = owner;
                draft.selected = -1;
            }
            else {
                Cancel (draft);
                draft.selected = near;
                design.selected = -1;
            }
        }
        if (design.dragging && design.owner == owner) {
            if (!ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
                design.dragging = false;
                design.owner = 0;
            }
            else if (hovered && ImGui::IsMouseDragging (ImGuiMouseButton_Left))
                MoveUnit (design, size_t (design.selected),
                          { worldMouse.x + design.dragOffset.x, worldMouse.y + design.dragOffset.y });
        }
        if (draft.dragging && draft.dragOwner == owner) {
            if (ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
                if (ImGui::IsMouseDragging (ImGuiMouseButton_Left))
                    Drag (*floor, draft, worldMouse);
            }
            else
                EndDrag (draft);
        }
        if (hovered && (draft.placing || draft.moving || near >= 0))
            ImGui::SetMouseCursor (draft.moving ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
        const auto rectangle = [&] (Point point, ImU32 colour, bool selected) {
            const auto a = project ({ point.x - kStairWidth / 2, point.y + kStairDepth / 2 });
            const auto b = project ({ point.x + kStairWidth / 2, point.y - kStairDepth / 2 });
            draw->AddRectFilled (a, b, (colour & 0x00FFFFFFu) | 0x30000000u);
            draw->AddRect (a, b, colour, 0, 0, (selected ? 3 : 1.5f) * scale);
        };
        for (size_t i = 0; i < draft.points.size (); ++i) {
            const auto at = project (draft.points[i]);
            const ImU32 colour =
                Fits (*floor, draft.points[i]) ? IM_COL32 (255, 186, 0, 255) : IM_COL32 (229, 72, 77, 255);
            rectangle (draft.points[i], colour, draft.selected == int (i));
            draw->AddText ({ at.x + 3 * scale, at.y }, colour, std::to_string (i + 1).c_str ());
        }
        if (hovered && draft.placing)
            rectangle (Snap (*floor, worldMouse),
                       Fits (*floor, Snap (*floor, worldMouse)) ? IM_COL32 (255, 186, 0, 200)
                                                                : IM_COL32 (229, 72, 77, 200),
                       false);
        if (design.stage == 6 && design.ready)
            for (size_t i = 0; i < design.seeds.size (); ++i) {
                const auto center = project (UnitCenter (design, design.seeds[i]));
                const auto colour = hudshell::Packed (UnitColour (design.seeds[i].rooms));
                draw->AddCircleFilled (center, (design.selected == int (i) ? 5 : 3) * scale, colour);
                const auto& seed = design.seeds[i];
                const std::string label =
                    "U" + std::to_string (seed.id) + " / " + hudmeta::NumberText (seed.rooms) + "R";
                draw->AddText ({ center.x + 6 * scale, center.y }, colour, label.c_str ());
            }
        if (hovered && (nearUnit >= 0 || design.dragging)) {
            ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
            ImGui::SetTooltip ("Drag centre along its segment to nudge apartment boundaries (0.3 m). Escape cancels.");
        }
        draw->PopClipRect ();
        ImGui::TextDisabled ("Blue: counted floor | Gray: physical outline | XY: project metres");
    }
    for (size_t i = 0; i < draft.points.size (); ++i) {
        ImGui::PushID (int (i));
        const std::string label = "Stair " + std::to_string (i + 1) + ": X " + hudmeta::NumberText (draft.points[i].x) +
                                  ", Y " + hudmeta::NumberText (draft.points[i].y);
        if (ImGui::Selectable (label.c_str (), draft.selected == int (i))) {
            Cancel (draft);
            draft.selected = int (i);
            draft.placing = false;
        }
        if (!Fits (*floor, draft.points[i]))
            ImGui::TextWrapped ("Footprint does not fit this floor; relocate if required. Shared centre retained.");
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
                                                     [&] (Point point) { return Fits (*floor, point); }));
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
            Cancel (draft);
            draft.points.clear ();
            draft.changed = true;
            draft.selected = -1;
            draft.placing = false;
        }
        ImGui::EndDisabled ();
    }
    ImGui::BeginDisabled (!Dirty (draft) || conflict || draft.dragging);
    if (ImGui::Button ("Save stairwells")) {
        edits = Edits (plan, draft);
        Cancel (draft);
    }
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::Button ("Discard"))
        Reset (plan, draft);
    ImGui::TextDisabled ("%s | Stairs saved as metadata; apartment schemes are session-local.",
                         Dirty (draft) ? "Local changes not yet saved" : "No local changes");
    const auto& info = QuickFor (plan, draft, *floor);
    ImGui::TextWrapped ("%s", info.note.c_str ());
    if (info.selected >= 0 && size_t (info.selected) < info.units.size ()) {
        const auto& seed = info.seeds[size_t (info.selected)];
        ImGui::Text ("U%u | %.1f rooms | %.1f m2 allocated / %.1f m2 target", seed.id, seed.rooms,
                     UnitArea (info.units[size_t (info.selected)]), UnitTargetArea (seed.rooms));
        ImGui::TextDisabled ("Weights nudge shared boundaries; target areas and room layouts are not guaranteed.");
    }
    ImGui::TextDisabled ("%s | Colours: room targets | Tan: corridors | Unassigned floor stays blue",
                         draft.uniqueFloors.contains (floor->story) ? "Unique floor" : "Shared by identical outlines");
    return edits;
}
} // namespace geomsrv::archviz::buildingplan
