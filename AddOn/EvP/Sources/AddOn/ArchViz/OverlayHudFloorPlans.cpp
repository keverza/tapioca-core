#include "ArchViz/OverlayHudEngine.hpp"
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::overlayhud {
namespace {
namespace fs = floorscheme;

void Line (overlaylayers::Layer& layer, const fs::Ring& ring, double z, uint32_t rgba, float width)
{
    overlaylayers::Polyline poly;
    poly.closed = true;
    poly.rgba = rgba;
    poly.widthPixels = width;
    poly.behind = overlaylayers::Behind::Show;
    for (const auto& p : ring)
        poly.points.insert (poly.points.end (), { p.x, p.y, z });
    layer.polylines.push_back (std::move (poly));
}
void Box (overlaylayers::Layer& layer, const fs::Ring& ring, double z0, double z1)
{
    if (ring.size () != 4)
        return;
    overlaylayers::Mesh box;
    box.rgba = 0x969696FFu;
    box.styled = true;
    box.style.shading = overlaylayers::Shading::Lit;
    box.style.behind = overlaylayers::Behind::Show;
    for (double z : { z0, z1 })
        for (const auto& p : ring)
            box.points.insert (box.points.end (), { p.x, p.y, z });
    box.indices = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                    1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7 };
    layer.meshes.push_back (std::move (box));
}
} // namespace

// The floors' schemes on the overlay: every building's flats, circulation and party walls at
// their floors, its stairs as boxes a floor high, and the flat selected in a Plan view white.
// Planned off the UI thread (FloorPlanner.hpp): asked when what a floor is planned from changes,
// published when a scheme arrives or the selection moves.
bool TakeFloorPlanLayers (State& state, overlaylayers::Layer& stairs, overlaylayers::Layer& units)
{
    using namespace buildingplan;
    fs::Options options;
    options.grossFactor = state.massingCoefficients.grossFactor;
    std::ostringstream asked;
    asked << std::setprecision (12) << floorprogramme::Key (state.massingProgramme) << options.grossFactor;
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        auto& draft = state.buildingPlans[key];
        Sync (plan, draft);
        UseProgramme (draft, state.massingProgramme);
        asked << key << ':' << fs::edit::ToJson (draft.designs) << ':';
        for (const auto& core : draft.cores)
            asked << core.center.x << ',' << core.center.y << ',' << core.width << ',' << core.depth << ';';
        for (const auto& floor : plan.floors)
            asked << floor.story << '@' << floor.z << floor.outlineKey.size () << floor.outlineKey.substr (0, 64);
    }
    const bool arrived = state.floorPlanner.Poll ();
    const bool wanted = state.previewStairs || state.previewUnits;
    if (wanted && (arrived || asked.str () != state.floorPlanAsked)) {
        state.floorPlanAsked = asked.str ();
        for (const auto& [key, plan] : state.floorPlanSnapshots)
            WantFloors (state.floorPlanner, state.floorPlanSnapshots, state.buildingPlans, key, state.massingProgramme,
                        state.buildingPlans[key].story, options);
    }
    std::vector<hudfloorscheme::Selection> selected;
    for (const auto& [key, editor] : state.planEditors)
        if (auto s = hudfloorscheme::Selected (editor))
            selected.push_back (std::move (*s));
    std::ostringstream signature;
    signature << std::setprecision (17) << state.previewStairs << state.previewUnits << ':'
              << state.floorPlanner.Revision ();
    for (const auto& [key, plan] : state.floorPlanSnapshots)
        signature << '|' << key << ':' << plan.floors.size ();
    for (const auto& s : selected)
        signature << '|' << s.building << '#' << s.story << '@' << s.flat.front ().x << ',' << s.flat.front ().y;
    const auto published = signature.str ();
    if (published == state.floorPlanPublished)
        return false;
    state.floorPlanPublished = published;
    stairs = {};
    units = {};
    stairs.name = kStairsLayer;
    units.name = kUnitsLayer;
    for (const auto& [key, plan] : state.floorPlanSnapshots)
        for (const auto& floor : plan.floors) {
            const auto* planned = state.floorPlanner.Latest (FloorId (key, floor.story));
            if (!planned)
                continue;
            const auto& s = planned->scheme;
            const double z = floor.z + 0.02;
            if (state.previewStairs)
                for (const auto& core : s.cores)
                    Box (stairs, core.shape, floor.z, floor.z + (std::max) (0.0, floor.height));
            if (!state.previewUnits)
                continue;
            for (const auto& f : s.flats)
                Line (units, f.shape, z, floorprogramme::Colour (f.rooms), 2.0f);
            for (const auto& c : s.corridors)
                Line (units, c.shape, z, 0xD6C49AFFu, 2.0f);
            for (const auto& w : s.party) {
                overlaylayers::Polyline wall;
                wall.rgba = 0x282C30FFu;
                wall.widthPixels = 5.0f;
                wall.behind = overlaylayers::Behind::Show;
                wall.points = { w[0].x, w[0].y, z, w[1].x, w[1].y, z };
                units.polylines.push_back (std::move (wall));
            }
        }
    // The flat selected in a Plan view, white over the rest, whether outlines are shown or not.
    for (const auto& s : selected)
        if (const auto plan = state.floorPlanSnapshots.find (s.building); plan != state.floorPlanSnapshots.end ())
            for (const auto& floor : plan->second.floors)
                if (floor.story == s.story)
                    Line (units, s.flat, floor.z + 0.04, 0xFFFFFFFFu, 5.0f);
    return true;
}
} // namespace geomsrv::archviz::overlayhud
