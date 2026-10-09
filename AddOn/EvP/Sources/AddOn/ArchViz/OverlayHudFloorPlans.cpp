#include "ArchViz/OverlayHudEngine.hpp"
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::overlayhud {
bool TakeFloorPlanLayers (State& state, overlaylayers::Layer& stairs, overlaylayers::Layer& units)
{
    using namespace buildingplan;
    std::ostringstream signature;
    signature << std::setprecision (17) << state.previewStairs << state.previewUnits;
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        auto& draft = state.buildingPlans[key];
        Sync (plan, draft);
        signature << key << ':' << Conflict (plan, draft);
        if (Conflict (plan, draft))
            continue;
        for (const auto& floor : plan.floors) {
            signature << ':' << floor.z << ':' << floor.height << QuickSignature (floor, draft.points);
            if (state.previewUnits) {
                const auto& quick = QuickFor (plan, draft, floor);
                signature << ':' << quick.revision << ':' << quick.signature << ':'
                          << draft.uniqueFloors.contains (floor.story);
            }
        }
    }
    const auto wanted = signature.str ();
    if (wanted == state.floorPlanPublished)
        return false;
    state.floorPlanPublished = wanted;
    stairs = {};
    units = {};
    stairs.name = kStairsLayer;
    units.name = kUnitsLayer;
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        overlaylayers::Layer coreLayer, unitLayer;
        if (state.previewStairs || state.previewUnits)
            PreviewLayers (plan, state.buildingPlans[key], coreLayer, unitLayer, state.previewUnits);
        if (state.previewStairs) {
            stairs.polylines.insert (stairs.polylines.end (), coreLayer.polylines.begin (), coreLayer.polylines.end ());
            stairs.meshes.insert (stairs.meshes.end (), coreLayer.meshes.begin (), coreLayer.meshes.end ());
        }
        if (state.previewUnits)
            units.polylines.insert (units.polylines.end (), unitLayer.polylines.begin (), unitLayer.polylines.end ());
    }
    return true;
}
} // namespace geomsrv::archviz::overlayhud
