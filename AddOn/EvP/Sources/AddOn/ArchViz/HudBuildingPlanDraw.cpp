#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::buildingplan {
namespace {
std::string Metres (double value)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%.1f", value);
    return text;
}
// Width and depth of the selected core, or of the next one placed.
void CoreSize (const Floor& floor, const Plan& plan, Draft& draft, float scale)
{
    const bool chosen = draft.selected >= 0 && size_t (draft.selected) < draft.cores.size ();
    Core& core = chosen ? draft.cores[size_t (draft.selected)] : draft.newCore;
    const Core before = core;
    ImGui::AlignTextToFramePadding ();
    ImGui::TextUnformatted (chosen ? "Core" : "New core");
    const double low = kMinCore, high = kMaxCore;
    for (auto* value : { &core.width, &core.depth }) {
        ImGui::SameLine ();
        ImGui::PushID (value);
        ImGui::SetNextItemWidth (60 * scale);
        ImGui::DragScalar ("##size", ImGuiDataType_Double, value, 0.05f, &low, &high, "%.2f m",
                           ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID ();
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Rotate"))
        std::swap (core.width, core.depth);
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Swap width and depth: turn the core a quarter in the building frame.");
    ImGui::SameLine ();
    if (ImGui::SmallButton ("4.5x4.2"))
        core = { core.center, kStairWidth, kStairDepth };
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Stair around a lift: 4.5 m along the corridor, 4.2 m deep.");
    ImGui::SameLine ();
    if (ImGui::SmallButton ("9.0x2.5"))
        core = { core.center, 9.0, 2.5 };
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Straight stair along the corridor, entered from the middle of its long side.");
    core.width = std::round (core.width * 20) / 20;
    core.depth = std::round (core.depth * 20) / 20;
    if (chosen && core != before) {
        Cancel (draft);
        draft.changed = true;
        if (!Fits (floor, core, plan.angle))
            ImGui::TextDisabled ("The resized core does not fit this floor; move it or change the size.");
    }
}
void TypeCombo (QuickPlan& design, size_t& type, float scale)
{
    ImGui::SetNextItemWidth (110 * scale);
    if (!ImGui::BeginCombo ("##unitType", floorprogramme::Name (design.programme, type).c_str ()))
        return;
    for (size_t t = 0; t < design.programme.types.size (); ++t) {
        const auto& item = design.programme.types[t];
        char label[96];
        std::snprintf (label, sizeof (label), "%s  %.0f-%.0f m2", floorprogramme::Name (design.programme, t).c_str (),
                       item.minM2, item.maxM2);
        if (ImGui::Selectable (label, t == type))
            type = t;
    }
    ImGui::EndCombo ();
}
} // namespace
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
    ImGui::TextDisabled ("Cores turn with the building frame (%.1f deg) | Snap within 0.5 m",
                         std::remainder (plan.angle * 180 / 3.14159265358979323846, 180.0));

    const bool active = draft.placing || draft.moving;
    ImGui::BeginDisabled (!active && (conflict || draft.cores.size () >= kMaxStairs));
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
    if (draft.selected >= 0 && size_t (draft.selected) < draft.cores.size ()) {
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
            draft.cores.erase (draft.cores.begin () + draft.selected);
            draft.selected = -1;
            draft.placing = false;
            draft.changed = true;
        }
        ImGui::EndDisabled ();
    }
    ImGui::BeginDisabled (conflict || draft.dragging);
    CoreSize (*floor, plan, draft, scale);
    ImGui::EndDisabled ();
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
    auto& controls = QuickFor (plan, draft, *floor);
    if (controls.stage == 6 && controls.ready) {
        ImGui::BeginDisabled (conflict || draft.placing || draft.moving || controls.dragging);
        if (ImGui::SmallButton ("Regenerate"))
            Regenerate (plan, draft, *floor);
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Fresh programme fill around the locked flats, then Optimise without moving cores.");
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Optimise"))
            Optimise (plan, draft, *floor);
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Retype, add, remove or reorder unlocked flats for the programme ranges and mix, larger "
                               "flats on corners, entrances and egress. Locked flats keep type, size and traits.");
        ImGui::SameLine ();
        ImGui::Checkbox ("may move cores", &draft.moveCores);
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Export plan"))
            draft.exportRequested = true;
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Save this building's floors, cores and flat designs as a JSON reference in the local "
                               "Tapioca data folder (plans). The private generator opens it as a fixture.");
        ImGui::EndDisabled ();
    }
    auto& units = QuickFor (plan, draft, *floor);
    if (units.stage == 6 && units.ready) {
        ImGui::BeginDisabled (conflict || draft.placing || draft.moving || units.dragging);
        if (ImGui::SmallButton (units.adding ? "Cancel unit" : "Add unit"))
            units.adding = !units.adding;
        ImGui::SameLine ();
        ImGui::BeginDisabled (units.selected < 0);
        if (ImGui::SmallButton ("Delete unit"))
            RemoveUnit (units, size_t (units.selected));
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        size_t type = units.selected >= 0 ? units.seeds[size_t (units.selected)].type : units.newType;
        const size_t was = type;
        TypeCombo (units, type, scale);
        if (type != was) {
            units.newType = type;
            if (units.selected >= 0)
                ChangeUnitTarget (plan, draft, *floor, size_t (units.selected), type,
                                  units.seeds[size_t (units.selected)].locked);
        }
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Programme type of the selected flat (its net target is the middle of the range); "
                               "neighbours relax. New flats use this type.");
        ImGui::BeginDisabled (units.selected < 0);
        const bool locked = units.selected >= 0 && units.seeds[size_t (units.selected)].locked;
        if (ImGui::SmallButton (locked ? "Unlock flat" : "Lock flat")) {
            if (locked)
                SetUnitLocked (units, size_t (units.selected), false);
            else
                ChangeUnitTarget (plan, draft, *floor, size_t (units.selected),
                                  units.seeds[size_t (units.selected)].type, true);
        }
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Keep this flat's type and net area, and where it is: corner, dual aspect or straight "
                               "facade. Neighbours, unlocked flats and proposed cores may move.");
        ImGui::EndDisabled ();
        ImGui::EndDisabled ();
    }
    auto& shown = QuickFor (plan, draft, *floor);
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
        const auto fill = [&] (const PlanRegion& region, ImU32 colour) {
            for (size_t i = 0; i + 2 < region.triangles.size (); i += 3)
                draw->AddTriangleFilled (project (region.triangles[i]), project (region.triangles[i + 1]),
                                         project (region.triangles[i + 2]), colour);
        };
        if (shown.stage == 6) {
            for (const auto& corridor : shown.corridors)
                fill (corridor, IM_COL32 (214, 196, 154, 255));
            for (size_t i = 0; i < shown.units.size (); ++i)
                fill (shown.units[i], hudshell::Packed (UnitColour (Rooms (shown, shown.seeds[i]))));
            for (const auto& lost : shown.unassigned)
                fill (lost, IM_COL32 (229, 72, 77, 140));
        }
        if (shown.stage == 1 && !shown.bars.empty ()) {
            const auto o = project (shown.origin);
            draw->AddLine (
                o,
                project ({ shown.origin.x + 5 * std::cos (shown.angle), shown.origin.y + 5 * std::sin (shown.angle) }),
                IM_COL32 (229, 72, 77, 255), 2 * scale);
            draw->AddLine (
                o,
                project ({ shown.origin.x - 5 * std::sin (shown.angle), shown.origin.y + 5 * std::cos (shown.angle) }),
                IM_COL32 (127, 200, 169, 255), 2 * scale);
        }
        if (shown.stage == 2)
            regions (shown.bars, IM_COL32 (176, 136, 201, 255));
        if (shown.stage >= 3)
            regions (shown.corridors, IM_COL32 (214, 196, 154, 255));
        if (shown.stage == 4)
            regions (shown.bands, IM_COL32 (154, 209, 230, 255));
        if (shown.stage == 5 && shown.ready)
            regions (shown.segments, IM_COL32 (74, 127, 181, 255));
        if (shown.stage == 6) {
            for (size_t i = 0; i < shown.units.size (); ++i)
                outlines (std::vector<std::vector<SliceChain>> { shown.units[i].rings }, IM_COL32 (38, 57, 57, 255),
                          1.5f * scale);
            for (const auto& cell : shown.egress.invalid)
                draw->AddCircleFilled (project (cell), 1.5f * scale, IM_COL32 (229, 72, 77, 255));
        }
        int near = -1;
        double best = 1e300;
        const auto mouse = ImGui::GetIO ().MousePos;
        const auto worldMouse = unproject (mouse);
        for (size_t i = 0; i < draft.cores.size (); ++i) {
            const auto& core = draft.cores[i];
            const double x = worldMouse.x - core.center.x, y = worldMouse.y - core.center.y;
            const double u = x * std::cos (plan.angle) + y * std::sin (plan.angle);
            const double v = -x * std::sin (plan.angle) + y * std::cos (plan.angle);
            const double distance = std::hypot (x, y);
            if (std::abs (u) <= core.width / 2 && std::abs (v) <= core.depth / 2 && distance < best) {
                best = distance;
                near = int (i);
            }
        }
        int nearUnit = -1;
        float unitDistance = 10 * scale;
        if (shown.stage == 6 && shown.ready)
            for (size_t i = 0; i < shown.seeds.size (); ++i) {
                const auto center = project (UnitCenter (shown, shown.seeds[i]));
                const float distance = std::hypot (center.x - mouse.x, center.y - mouse.y);
                if (distance < unitDistance) {
                    unitDistance = distance;
                    nearUnit = int (i);
                }
            }
        auto& design = shown;
        if (hovered && !conflict && ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
            if (draft.placing)
                Place (*floor, draft, worldMouse, plan.angle);
            else if (draft.moving && near == draft.selected)
                BeginDrag (draft, worldMouse, owner, plan.angle);
            else if (design.stage == 6 && design.adding)
                AddUnit (design, worldMouse);
            else if (design.stage == 6 && nearUnit >= 0 && near < 0) {
                Cancel (draft);
                design.selected = nearUnit;
                design.dragOriginal = design.seeds[size_t (nearUnit)].along;
                design.dragSeeds = design.seeds;
                design.dragUnits = design.units;
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
                design.dragSeeds.clear ();
                design.dragUnits.clear ();
            }
            else if (hovered && ImGui::IsMouseDragging (ImGuiMouseButton_Left))
                MoveUnit (design, size_t (design.selected),
                          { worldMouse.x + design.dragOffset.x, worldMouse.y + design.dragOffset.y });
        }
        if (draft.dragging && draft.dragOwner == owner) {
            if (ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
                if (ImGui::IsMouseDragging (ImGuiMouseButton_Left))
                    Drag (*floor, draft, worldMouse, plan.angle);
            }
            else
                EndDrag (draft);
        }
        if (hovered && (draft.placing || draft.moving || near >= 0))
            ImGui::SetMouseCursor (draft.moving ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
        const auto rectangle = [&] (const Core& core, ImU32 colour, bool selected) {
            ImVec2 corners[4];
            const auto world = Corners (core, plan.angle);
            for (size_t i = 0; i < 4; ++i)
                corners[i] = project (world[i]);
            draw->AddConvexPolyFilled (corners, 4, IM_COL32 (150, 150, 150, 255));
            draw->AddPolyline (corners, 4, colour, ImDrawFlags_Closed, (selected ? 3 : 1.5f) * scale);
        };
        for (size_t i = 0; i < draft.cores.size (); ++i) {
            const auto at = project (draft.cores[i].center);
            const ImU32 colour =
                Fits (*floor, draft.cores[i], plan.angle) ? IM_COL32 (255, 186, 0, 255) : IM_COL32 (229, 72, 77, 255);
            rectangle (draft.cores[i], colour, draft.selected == int (i));
            draw->AddText ({ at.x + 3 * scale, at.y }, colour, std::to_string (i + 1).c_str ());
        }
        if (hovered && draft.placing) {
            Core ghost = draft.newCore;
            ghost.center = worldMouse;
            ghost.center = Snap (*floor, ghost, plan.angle);
            rectangle (ghost,
                       Fits (*floor, ghost, plan.angle) ? IM_COL32 (255, 186, 0, 200) : IM_COL32 (229, 72, 77, 200),
                       false);
        }
        if (design.stage == 6 && design.ready)
            for (size_t i = 0; i < design.seeds.size (); ++i) {
                const auto center = project (UnitCenter (design, design.seeds[i]));
                const auto colour = hudshell::Packed (UnitColour (Rooms (design, design.seeds[i])));
                draw->AddCircleFilled (center, (design.selected == int (i) ? 5 : 3) * scale,
                                       IM_COL32 (30, 40, 40, 255));
                if (design.selected == int (i))
                    draw->AddCircle (center, 7 * scale, colour, 0, 2 * scale);
                const auto& seed = design.seeds[i];
                const std::string label =
                    "U" + std::to_string (seed.id) + " / " + TypeName (design, seed) + (seed.locked ? " [L]" : "");
                draw->AddText ({ center.x + 6 * scale, center.y }, IM_COL32 (30, 40, 40, 255), label.c_str ());
            }
        if (hovered && (nearUnit >= 0 || design.dragging)) {
            ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
            ImGui::SetTooltip (
                "Drag to relax neighbours along the band (0.3 m); locked sizes stay exact. Escape cancels.");
        }
        draw->PopClipRect ();
        ImGui::TextDisabled ("Blue: counted floor | Gray: physical outline | Red: empty floor, egress beyond limit");
    }
    for (size_t i = 0; i < draft.cores.size (); ++i) {
        ImGui::PushID (int (i));
        const auto& core = draft.cores[i];
        const std::string label = "Stair " + std::to_string (i + 1) + ": X " + hudmeta::NumberText (core.center.x) +
                                  ", Y " + hudmeta::NumberText (core.center.y) + " | " + Metres (core.width) + " x " +
                                  Metres (core.depth) + " m";
        if (ImGui::Selectable (label.c_str (), draft.selected == int (i))) {
            Cancel (draft);
            draft.selected = int (i);
            draft.placing = false;
        }
        if (!Fits (*floor, core, plan.angle))
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
        const size_t inside = size_t (std::count_if (draft.cores.begin (), draft.cores.end (), [&] (const Core& core) {
            return Fits (*floor, core, plan.angle);
        }));
        ImGui::TextWrapped ("Proposed building-wide: %zu stairs (%zu inside this floor). Approximate locations only.",
                            draft.cores.size (), inside);
    }
    if (plan.mixed)
        ImGui::TextWrapped ("Members have different or invalid saved stairwell metadata. Add points and Save to "
                            "replace it for the whole building.");
    if (conflict)
        ImGui::TextWrapped ("Building membership or saved locations changed. Discard to reload before saving.");
    if (plan.mixed || !draft.cores.empty ()) {
        ImGui::BeginDisabled (conflict);
        if (ImGui::SmallButton ("Clear all proposed stairs")) {
            Cancel (draft);
            draft.cores.clear ();
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
    auto& info = QuickFor (plan, draft, *floor);
    ImGui::TextWrapped ("%s", info.note.c_str ());
    if (!info.solveNote.empty ())
        ImGui::TextWrapped ("%s", info.solveNote.c_str ());
    if (info.ready) {
        std::vector<int> counts (info.programme.types.size (), 0);
        double net = 0;
        for (const auto& seed : info.seeds) {
            if (seed.type < counts.size ())
                ++counts[seed.type];
            net += Traits (info, seed).net;
        }
        std::string mix;
        for (size_t t = 0; t < counts.size (); ++t)
            mix += (t ? ", " : "") + floorprogramme::Name (info.programme, t) + " " + std::to_string (counts[t]);
        ImGui::TextWrapped ("Flats %zu | net %.1f m2 | %s", info.seeds.size (), net, mix.c_str ());
        double empty = 0;
        for (const auto& region : info.unassigned)
            empty += UnitArea (region);
        ImGui::TextDisabled ("Score %.1f (lower is better) | empty %.1f m2 | farthest corridor point %.1f m%s",
                             info.score, empty, info.egress.longest,
                             info.egress.invalid.empty () ? "" : " | beyond egress limit (red)");
    }
    if (info.selected >= 0 && size_t (info.selected) < info.units.size ()) {
        auto& seed = info.seeds[size_t (info.selected)];
        const auto traits = Traits (info, seed);
        const auto& type = info.programme.types[(std::min) (seed.type, info.programme.types.size () - 1)];
        ImGui::Text ("U%u | %s | net %.1f m2 of %.0f-%.0f | facade %.1f m", seed.id, TypeName (info, seed).c_str (),
                     traits.net, type.minM2, type.maxM2, traits.facade);
        ImGui::BeginDisabled (!seed.locked);
        uint8_t keep = seed.keep;
        for (const auto& [trait, label] : { std::pair { kCorner, "Corner" },
                                            { kDualAspect, "Dual aspect" },
                                            { kStraightFacade, "Straight facade" } }) {
            bool on = (keep & trait) != 0;
            const std::string text = std::string (label) + ((traits.traits & trait) ? " (has)" : " (lacks)");
            if (ImGui::Checkbox (text.c_str (), &on))
                keep = on ? uint8_t (keep | trait) : uint8_t (keep & ~trait);
            ImGui::SameLine ();
        }
        ImGui::NewLine ();
        ImGui::EndDisabled ();
        if (keep != seed.keep)
            SetUnitKeep (info, size_t (info.selected), keep);
        ImGui::TextDisabled ("%s", seed.locked ? "Locked: Regenerate and Optimise keep type, size and ticked traits."
                                               : "Unlocked: Optimise may retype, move or remove it.");
    }
    ImGui::TextDisabled ("%s | Colours: rooms | Tan: circulation",
                         draft.uniqueFloors.contains (floor->story) ? "Unique floor" : "Shared by identical outlines");
    return edits;
}
} // namespace geomsrv::archviz::buildingplan
