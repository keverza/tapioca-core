#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/BuildingTopology.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <climits>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::overlayhud {
namespace {
namespace fs = floorscheme;
namespace bt = buildingtopology;
constexpr uint32_t kPlanFillAlpha = 0x59;  // a flat's fill: its colour at 35 %
constexpr double kEdgeLabelShortest = 0.6; // m: a side shorter carries no length

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
// Each side of `ring` its length, lying on the floor along it just inside, as the slices' areas lie
// on them; readable only zoomed in (user, 2026-10-15: "if zoomed in in overlay show segment length
// alligned to line like we have for area display").
void EdgeLengths (overlaylayers::Layer& layer, const fs::Ring& ring, double z)
{
    double area = 0;
    for (size_t i = 0; i < ring.size (); ++i)
        area += ring[i].x * ring[(i + 1) % ring.size ()].y - ring[(i + 1) % ring.size ()].x * ring[i].y;
    const double inward = area >= 0 ? 1.0 : -1.0; // the left of an anticlockwise side is inside
    for (size_t i = 0; i < ring.size (); ++i) {
        const auto& a = ring[i];
        const auto& b = ring[(i + 1) % ring.size ()];
        const double dx = b.x - a.x, dy = b.y - a.y, length = std::hypot (dx, dy);
        if (length < kEdgeLabelShortest)
            continue;
        overlaylayers::Text t;
        char text[32];
        std::snprintf (text, sizeof (text), "%.2f", length);
        t.text = text;
        t.planar = true;
        t.at[0] = (a.x + b.x) / 2 - inward * dy / length * 0.3;
        t.at[1] = (a.y + b.y) / 2 + inward * dx / length * 0.3;
        t.at[2] = z;
        // Read upright: along the side, left to right on the plan.
        const bool flip = dx < -1e-9 || (std::abs (dx) <= 1e-9 && dy < 0);
        t.direction[0] = (flip ? -dx : dx) / length;
        t.direction[1] = (flip ? -dy : dy) / length;
        t.direction[2] = 0;
        t.sizeMetres = 0.28;
        t.sizePixels = 32.0f; // the layout's resolution only
        t.minProjectedPixels = 9;
        // Black on a white halo, as the slices' areas are (StorySliceOverlayContent.hpp).
        t.rgba = 0x202020FFu;
        t.haloRgba = 0xFFFFFFD0u;
        t.behind = overlaylayers::Behind::Show;
        layer.texts.push_back (std::move (t));
    }
}
// The building's main walls (floorscheme::MainWalls of its stack floor; user, 2026-10-15: "GridLines
// may be shown in overlay through show toggle in Massing Tab"). On the plan, at `z`: an axis 1.5 m
// past both facades, dashed, numbered in a circle beyond one end. In 3D, from `z0` to `z1`: each a
// dashed frame standing from the lowest floor to the top, as the wall does.
constexpr uint32_t kGridRgba = 0xC44A40E0u;
void PlanGrid (overlaylayers::Layer& layer, const std::vector<std::array<fs::Vec, 2>>& grid, double z)
{
    for (size_t i = 0; i < grid.size (); ++i) {
        const auto& g = grid[i];
        const double dx = g[1].x - g[0].x, dy = g[1].y - g[0].y, len = std::hypot (dx, dy);
        if (len < 1e-6)
            continue;
        const fs::Vec u { dx / len, dy / len };
        overlaylayers::Polyline axis;
        axis.rgba = kGridRgba;
        axis.widthPixels = 1.5f;
        axis.dashMetres = { 0.8f, 0.25f };
        axis.behind = overlaylayers::Behind::Show;
        axis.points = { g[0].x - u.x * 1.5, g[0].y - u.y * 1.5, z, g[1].x + u.x * 1.5, g[1].y + u.y * 1.5, z };
        layer.polylines.push_back (std::move (axis));
        const fs::Vec c { g[1].x + u.x * 2.1, g[1].y + u.y * 2.1 };
        overlaylayers::Polyline bubble;
        bubble.closed = true;
        bubble.rgba = kGridRgba;
        bubble.widthPixels = 1.5f;
        bubble.behind = overlaylayers::Behind::Show;
        for (int k = 0; k < 24; ++k) {
            const double a = k * 2 * 3.14159265358979 / 24;
            bubble.points.insert (bubble.points.end (), { c.x + 0.5 * std::cos (a), c.y + 0.5 * std::sin (a), z });
        }
        layer.polylines.push_back (std::move (bubble));
        overlaylayers::Text number;
        number.text = std::to_string (i + 1);
        number.planar = true;
        number.at[0] = c.x, number.at[1] = c.y, number.at[2] = z;
        number.sizeMetres = 0.5;
        number.sizePixels = 32.0f;
        number.rgba = kGridRgba | 0xFFu;
        number.haloPixels = 0;
        number.behind = overlaylayers::Behind::Show;
        layer.texts.push_back (std::move (number));
    }
}
void FramedGrid (overlaylayers::Layer& layer, const std::vector<std::array<fs::Vec, 2>>& grid, double z0, double z1)
{
    for (const auto& g : grid) {
        overlaylayers::Polyline frame;
        frame.closed = true;
        frame.rgba = kGridRgba;
        frame.widthPixels = 1.5f;
        frame.dashMetres = { 0.8f, 0.25f };
        frame.behind = overlaylayers::Behind::Show;
        frame.points = { g[0].x, g[0].y, z0, g[1].x, g[1].y, z0, g[1].x, g[1].y, z1, g[0].x, g[0].y, z1 };
        layer.polylines.push_back (std::move (frame));
    }
}
// A ring filled, flat at `z`: triangulated, so an L or a notched flat fills only itself.
void Fill (overlaylayers::Layer& layer, const fs::Ring& ring, double z, uint32_t rgba)
{
    namespace cp = Clipper2Lib;
    cp::PathD path;
    for (const auto& p : ring)
        path.emplace_back (p.x, p.y);
    cp::PathsD triangles;
    if (ring.size () < 3 || cp::Triangulate ({ path }, 4, triangles, false) != cp::TriangulateResult::success)
        return;
    overlaylayers::Mesh mesh;
    mesh.rgba = rgba;
    mesh.styled = true;
    mesh.style.behind = overlaylayers::Behind::Show;
    for (const auto& t : triangles) {
        if (t.size () != 3)
            continue;
        const uint32_t base = static_cast<uint32_t> (mesh.points.size () / 3);
        for (const auto& q : t)
            mesh.points.insert (mesh.points.end (), { q.x, q.y, z });
        mesh.indices.insert (mesh.indices.end (), { base, base + 1, base + 2 });
    }
    if (!mesh.indices.empty ())
        layer.meshes.push_back (std::move (mesh));
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
// ⚠️ THE PLAN'S STOREY IS THE FLOOR SHOWN (user, 2026-10-15: "Selected slabs story and actual
// archicad story should be linked"): when Archicad's storey changes, every building with a floor
// there shows and edits that floor -- in its Plan view, on the plan and in 3D.
void SetPlanStorey (State& state, int storey)
{
    if (storey != INT_MIN && storey != state.planStorey)
        for (const auto& [key, plan] : state.floorPlanSnapshots)
            if (std::any_of (plan.floors.begin (), plan.floors.end (),
                             [&] (const buildingplan::Floor& f) { return f.story == storey; }))
                state.buildingPlans[key].story = storey;
    state.planStorey = storey;
}
bool TakeFloorPlanLayers (State& state, overlaylayers::Layer& stairs, overlaylayers::Layer& units,
                          overlaylayers::Layer* onPlan)
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
    const bool wanted = state.previewStairs || state.previewUnits || state.previewGrid;
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
    signature << std::setprecision (17) << state.previewStairs << state.previewUnits << state.previewGrid << ':'
              << state.floorPlanner.Revision () << ':' << floorprogramme::ColoursKey (state.massingProgramme) << ':'
              << (onPlan ? state.planStorey : INT_MIN);
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        const auto* shown = Displayed (plan, state.buildingPlans[key]);
        signature << '|' << key << ':' << plan.floors.size () << '@' << (shown ? shown->story : INT_MIN);
    }
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
    // User, 2026-10-14: "Floor Plan overlay show all stories. units are visible but needs to solid
    // fill not just outline." The plan draws its own storey's floor, filled; 3D keeps every floor.
    if (onPlan) {
        *onPlan = {};
        onPlan->name = kPlanUnitsLayer;
        onPlan->views = overlaylayers::Views::TwoD;
        units.views = overlaylayers::Views::ThreeD;
    }
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        const auto complex = ComplexOf (state, key, plan, options);
        if (state.previewGrid && !plan.floors.empty ()) {
            std::vector<fs::Pins::Core> stack;
            std::vector<std::array<fs::Vec, 2>> grid;
            Stack (state.floorPlanner, key, plan, stack, &grid);
            double z0 = 1e18, z1 = -1e18;
            for (const auto& floor : plan.floors) {
                z0 = (std::min) (z0, floor.z), z1 = (std::max) (z1, floor.z + floor.height);
                if (onPlan && floor.story == state.planStorey)
                    PlanGrid (*onPlan, grid, floor.z + 0.06);
            }
            FramedGrid (units, grid, z0, z1);
        }
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
        // The floor its Plan view shows -- picked in the building section -- is the one edited on the
        // locked view: in 3D its flats are filled (user, 2026-10-14: "In 3d locked mode units are
        // selectable, but should have solid transparent fill not just contour").
        const auto* edited = Displayed (plan, state.buildingPlans[key]);
        for (size_t f = 0; f < complex.floors.size (); ++f) {
            const auto& floor = complex.floors[f];
            const double z = floor.z + 0.02;
            const bool filled = edited && floor.story == edited->story;
            // The plan's storey goes to the plan as well, its flats filled under their outlines.
            overlaylayers::Layer* here = onPlan && floor.story == state.planStorey ? onPlan : nullptr;
            for (int u : floor.units) {
                const uint32_t colour =
                    floorprogramme::Colour (state.massingProgramme, complex.units[u].type, complex.units[u].rooms);
                for (const auto& ring : bt::Clear (complex, complex.units[u].cells)) {
                    if (filled)
                        Fill (units, ring, z, (colour & 0xFFFFFF00u) | kPlanFillAlpha);
                    Line (units, ring, z, colour, 2.0f);
                    if (here) {
                        Fill (*here, ring, z, (colour & 0xFFFFFF00u) | kPlanFillAlpha);
                        Line (*here, ring, z, colour, 2.0f);
                    }
                }
            }
            for (int id : floor.cells) {
                const auto kind = complex.cells[id].kind;
                if (kind == bt::CellKind::Corridor || kind == bt::CellKind::Lobby)
                    for (const auto& ring : bt::Clear (complex, { id })) {
                        Line (units, ring, z, 0xD6C49AFFu, 2.0f);
                        if (here)
                            Line (*here, ring, z, 0xD6C49AFFu, 2.0f);
                    }
            }
            const auto* planned = state.floorPlanner.Latest (FloorId (key, floor.story));
            for (const auto& w : planned ? planned->scheme.party : std::vector<std::array<fs::Vec, 2>> {}) {
                overlaylayers::Polyline wall;
                wall.rgba = 0x282C30FFu;
                wall.widthPixels = 5.0f;
                wall.behind = overlaylayers::Behind::Show;
                wall.points = { w[0].x, w[0].y, z, w[1].x, w[1].y, z };
                if (here)
                    here->polylines.push_back (wall);
                units.polylines.push_back (std::move (wall));
            }
        }
    }
    // The flat selected in a Plan view, white over the rest, whether outlines are shown or not:
    // inside its walls, as the outlines are. Its floor's flats carry their sides' lengths, every
    // one of them (user, 2026-10-15: "show unit segment lenghts but for whole floor of units not
    // just one selection").
    for (const auto& s : selected) {
        const auto plan = state.floorPlanSnapshots.find (s.building);
        if (plan == state.floorPlanSnapshots.end ())
            continue;
        for (const auto& floor : plan->second.floors) {
            if (floor.story != s.story)
                continue;
            overlaylayers::Layer* here = onPlan && floor.story == state.planStorey ? onPlan : nullptr;
            std::vector<fs::Ring> rings { s.flat }, measured { s.flat };
            if (const auto* planned = state.floorPlanner.Latest (FloorId (s.building, floor.story))) {
                const auto complex = bt::Build ({ { floor.story, floor.z, floor.height, &planned->scheme, {} } });
                measured.clear ();
                for (const auto& unit : complex.units) {
                    const auto clear = bt::Clear (complex, unit.cells);
                    if (planned->scheme.flats[unit.flat].shape == s.flat)
                        rings = clear;
                    measured.insert (measured.end (), clear.begin (), clear.end ());
                }
            }
            for (const auto& ring : rings) {
                Line (units, ring, floor.z + 0.04, 0xFFFFFFFFu, 5.0f);
                if (here)
                    Line (*here, ring, floor.z + 0.04, 0xFFFFFFFFu, 5.0f);
            }
            for (const auto& ring : measured) {
                EdgeLengths (units, ring, floor.z + 0.05);
                if (here)
                    EdgeLengths (*here, ring, floor.z + 0.05);
            }
        }
    }
    return true;
}
} // namespace geomsrv::archviz::overlayhud
