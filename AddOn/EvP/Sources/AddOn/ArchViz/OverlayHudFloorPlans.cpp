#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/BuildingTopology.hpp"
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::overlayhud {
namespace {
namespace fs = floorscheme;
namespace bt = buildingtopology;

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
// A convex counter-clockwise ring raised from z0 to z1: bottom and top fans, then the sides.
bool Prism (overlaylayers::Layer& layer, const fs::Ring& ring, double z0, double z1)
{
    const uint32_t n = static_cast<uint32_t> (ring.size ());
    if (n < 3 || z1 <= z0)
        return false;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& a = ring[i];
        const auto& b = ring[(i + 1) % n];
        const auto& c = ring[(i + 2) % n];
        if ((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) < -1e-9)
            return false; // not convex: a fan would cover the notch
    }
    overlaylayers::Mesh box;
    box.rgba = 0x969696FFu;
    box.styled = true;
    box.style.shading = overlaylayers::Shading::Lit;
    box.style.behind = overlaylayers::Behind::Show;
    for (double z : { z0, z1 })
        for (const auto& p : ring)
            box.points.insert (box.points.end (), { p.x, p.y, z });
    for (uint32_t i = 1; i + 1 < n; ++i)
        box.indices.insert (box.indices.end (), { 0, i + 1, i, n, n + i, n + i + 1 });
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = (i + 1) % n;
        box.indices.insert (box.indices.end (), { i, k, n + k, i, n + k, n + i });
    }
    layer.meshes.push_back (std::move (box));
    return true;
}
// A building's planned floors as one cell complex (BuildingTopology.hpp), the floors not yet
// planned left out.
bt::Complex ComplexOf (State& state, const std::string& key, const buildingplan::Plan& plan, const fs::Options& options)
{
    using namespace buildingplan;
    std::vector<fs::Pins::Core> stack;
    Stack (state.floorPlanner, key, plan, stack);
    std::vector<const Floor*> floors;
    for (const auto& floor : plan.floors)
        floors.push_back (&floor);
    std::sort (floors.begin (), floors.end (), [] (const Floor* a, const Floor* b) { return a->z < b->z; });
    std::vector<bt::FloorInput> inputs;
    for (const auto* floor : floors) {
        const auto* planned = state.floorPlanner.Latest (FloorId (key, floor->story));
        if (!planned)
            continue;
        const auto input = InputFor (state.floorPlanSnapshots, state.buildingPlans, key, *floor, state.massingProgramme,
                                     stack, options);
        inputs.push_back ({ floor->story, floor->z, floor->height, &planned->scheme, input.party });
    }
    return bt::Build (inputs, stack);
}
} // namespace

// The floors' schemes on the overlay, inside their walls (user, 2026-10-10): every building's
// flats and circulation clear of the 0.5 m facade and 0.2 m partitions at their floor's level, its
// party walls, each stack stair as one shaft from the lowest floor to under the top floor's slab,
// and the flat selected in a Plan view white.
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
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        const auto complex = ComplexOf (state, key, plan, options);
        if (state.previewStairs) {
            // A stack stair from its lowest floor to its highest: the shaft never moves. A stair
            // the stack does not have (CheckStack reported it) stands on its floor alone.
            std::map<int, std::pair<int, int>> shafts; // stack stair: lowest and highest cell
            for (size_t i = 0; i < complex.cells.size (); ++i) {
                const auto& cell = complex.cells[i];
                if (cell.kind != bt::CellKind::Core)
                    continue;
                const int id = static_cast<int> (i);
                const auto& floor = complex.floors[cell.floor];
                const double z = floor.z;
                if (cell.stack < 0) {
                    const auto ring = bt::Clear (complex, { id });
                    if (ring.size () == 1)
                        Prism (stairs, ring.front (), bt::ClearSpan (floor).z0, bt::ClearSpan (floor).z1);
                    continue;
                }
                auto [it, fresh] = shafts.try_emplace (cell.stack, id, id);
                if (!fresh) {
                    if (z < complex.floors[complex.cells[it->second.first].floor].z)
                        it->second.first = id;
                    if (z > complex.floors[complex.cells[it->second.second].floor].z)
                        it->second.second = id;
                }
            }
            for (const auto& [stair, ends] : shafts) {
                const auto ring = bt::Clear (complex, { ends.first });
                if (ring.size () == 1)
                    Prism (stairs, ring.front (), bt::ClearSpan (complex.floors[complex.cells[ends.first].floor]).z0,
                           bt::ClearSpan (complex.floors[complex.cells[ends.second].floor]).z1);
            }
        }
        if (!state.previewUnits)
            continue;
        for (size_t f = 0; f < complex.floors.size (); ++f) {
            const auto& floor = complex.floors[f];
            const double z = floor.z + 0.02;
            for (int u : floor.units)
                for (const auto& ring : bt::Clear (complex, complex.units[u].cells))
                    Line (units, ring, z, floorprogramme::Colour (complex.units[u].rooms), 2.0f);
            for (int id : floor.cells) {
                const auto kind = complex.cells[id].kind;
                if (kind == bt::CellKind::Corridor || kind == bt::CellKind::Lobby)
                    for (const auto& ring : bt::Clear (complex, { id }))
                        Line (units, ring, z, 0xD6C49AFFu, 2.0f);
            }
            const auto* planned = state.floorPlanner.Latest (FloorId (key, floor.story));
            for (const auto& w : planned ? planned->scheme.party : std::vector<std::array<fs::Vec, 2>> {}) {
                overlaylayers::Polyline wall;
                wall.rgba = 0x282C30FFu;
                wall.widthPixels = 5.0f;
                wall.behind = overlaylayers::Behind::Show;
                wall.points = { w[0].x, w[0].y, z, w[1].x, w[1].y, z };
                units.polylines.push_back (std::move (wall));
            }
        }
    }
    // The flat selected in a Plan view, white over the rest, whether outlines are shown or not:
    // inside its walls, as the outlines are.
    for (const auto& s : selected) {
        const auto plan = state.floorPlanSnapshots.find (s.building);
        if (plan == state.floorPlanSnapshots.end ())
            continue;
        for (const auto& floor : plan->second.floors) {
            if (floor.story != s.story)
                continue;
            std::vector<fs::Ring> rings { s.flat };
            if (const auto* planned = state.floorPlanner.Latest (FloorId (s.building, floor.story))) {
                const auto complex = bt::Build ({ { floor.story, floor.z, floor.height, &planned->scheme, {} } });
                for (const auto& unit : complex.units)
                    if (planned->scheme.flats[unit.flat].shape == s.flat)
                        rings = bt::Clear (complex, unit.cells);
            }
            for (const auto& ring : rings)
                Line (units, ring, floor.z + 0.04, 0xFFFFFFFFu, 5.0f);
        }
    }
    return true;
}
} // namespace geomsrv::archviz::overlayhud
