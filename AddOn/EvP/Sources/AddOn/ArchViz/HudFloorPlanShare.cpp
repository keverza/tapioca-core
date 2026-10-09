// Floors with identical outlines share one quick design; Make unique and Reset plan split
// and rejoin them. Designs regenerate when the outline, cores or programme change, keeping
// the locked flats.
#include "ArchViz/HudFloorPlanFrame.hpp"
#include <algorithm>

namespace geomsrv::archviz::buildingplan {
int frame::TemplateStory (const Plan& plan, const Draft& draft, const Floor& floor)
{
    if (draft.uniqueFloors.contains (floor.story))
        return floor.story;
    const auto shape = QuickSignature (floor, {});
    for (const auto& other : plan.floors)
        if (!draft.uniqueFloors.contains (other.story) && QuickSignature (other, {}) == shape)
            return other.story;
    return floor.story;
}
void UseProgramme (Draft& draft, const floorprogramme::Programme& programme)
{
    if (floorprogramme::Valid (programme) && !(draft.programme == programme))
        draft.programme = programme;
}
QuickPlan& QuickFor (const Plan& plan, Draft& draft, const Floor& floor)
{
    const int story = frame::TemplateStory (plan, draft, floor);
    auto& quick = draft.quickPlans[story];
    auto committed = draft.cores;
    if (draft.dragging && draft.selected >= 0 && size_t (draft.selected) < committed.size ())
        committed[size_t (draft.selected)].center = draft.dragOriginal;
    const auto signature = QuickSignature (floor, committed, &draft.programme);
    if (quick.signature != signature) {
        const int stage = quick.stage;
        const bool replaced = !quick.signature.empty ();
        auto next = GenerateQuick (floor, committed, draft.programme, plan.angle);
        const bool locked =
            std::any_of (quick.seeds.begin (), quick.seeds.end (), [] (const UnitSeed& seed) { return seed.locked; });
        if (locked && quick.outlineSignature == next.outlineSignature) {
            if (!KeepLocked (quick, next)) {
                quick.signature = signature;
                quick.ready = false;
                quick.units.clear ();
                quick.corridors.clear ();
                quick.bands.clear ();
                quick.solveNote = "Core change cannot preserve locked sizes. Restore core locations or reset the plan.";
                ++quick.revision;
                return quick;
            }
            OptimiseUnits (next, 300);
        }
        quick = std::move (next);
        quick.stage = stage;
        if (replaced)
            quick.note += " Outline, cores or programme changed: unlocked flats regenerated.";
    }
    return quick;
}
void MakeUnique (const Plan& plan, Draft& draft, const Floor& floor)
{
    if (draft.uniqueFloors.contains (floor.story))
        return;
    const int shared = frame::TemplateStory (plan, draft, floor);
    auto copy = QuickFor (plan, draft, floor);
    CancelUnits (copy);
    draft.uniqueFloors.insert (floor.story);
    if (shared == floor.story)
        for (const auto& other : plan.floors)
            if (!draft.uniqueFloors.contains (other.story) &&
                QuickSignature (other, {}) == QuickSignature (floor, {})) {
                draft.quickPlans[frame::TemplateStory (plan, draft, other)] = copy;
                break;
            }
    draft.quickPlans[floor.story] = std::move (copy);
}
void ResetQuick (const Plan& plan, Draft& draft, const Floor& floor)
{
    // Reset a unique floor rejoins its shared outline; reset a shared floor regenerates that template.
    QuickPlan shared;
    bool hasShared = false;
    if (draft.uniqueFloors.contains (floor.story))
        for (const auto& other : plan.floors)
            if (!draft.uniqueFloors.contains (other.story) &&
                QuickSignature (other, {}) == QuickSignature (floor, {})) {
                shared = QuickFor (plan, draft, other);
                hasShared = true;
                break;
            }
    const bool unique = draft.uniqueFloors.erase (floor.story) != 0;
    draft.quickPlans.erase (floor.story);
    if (!unique)
        draft.quickPlans.erase (frame::TemplateStory (plan, draft, floor));
    else if (hasShared)
        draft.quickPlans[frame::TemplateStory (plan, draft, floor)] = std::move (shared);
    QuickFor (plan, draft, floor);
}
void PreviewLayers (const Plan& plan, Draft& draft, overlaylayers::Layer& stairs, overlaylayers::Layer& units,
                    bool includeUnits)
{
    if (Conflict (plan, draft))
        return;
    const auto line = [] (overlaylayers::Layer& layer, const SliceChain& ring, double z, uint32_t rgba) {
        overlaylayers::Polyline poly;
        poly.closed = true;
        poly.rgba = rgba;
        poly.behind = overlaylayers::Behind::Show;
        for (size_t i = 0; i < ring.Count (); ++i)
            poly.points.insert (poly.points.end (), { ring.xy[i * 2], ring.xy[i * 2 + 1], z });
        layer.polylines.push_back (std::move (poly));
    };
    for (const auto& floor : plan.floors) {
        for (const auto& core : draft.cores) {
            const auto corners = Corners (core, plan.angle);
            overlaylayers::Mesh box;
            box.rgba = Fits (floor, core, plan.angle) ? 0x969696FFu : 0xE5484DFFu;
            box.styled = true;
            box.style.shading = overlaylayers::Shading::Lit;
            box.style.behind = overlaylayers::Behind::Show;
            for (double z : { floor.z, floor.z + (std::max) (0.0, floor.height) })
                for (const auto& p : corners)
                    box.points.insert (box.points.end (), { p.x, p.y, z });
            box.indices = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                            1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7 };
            stairs.meshes.push_back (std::move (box));
        }
        if (!includeUnits)
            continue;
        const auto& quick = QuickFor (plan, draft, floor);
        if (!quick.ready)
            continue;
        for (size_t i = 0; i < quick.units.size (); ++i) {
            const uint32_t colour = UnitColour (Rooms (quick, quick.seeds[i]));
            for (const auto& ring : quick.units[i].rings)
                line (units, ring, floor.z + 0.02, colour);
        }
        for (const auto& corridor : quick.corridors)
            for (const auto& ring : corridor.rings)
                line (units, ring, floor.z + 0.02, 0xD6C49AFFu);
    }
}
} // namespace geomsrv::archviz::buildingplan
