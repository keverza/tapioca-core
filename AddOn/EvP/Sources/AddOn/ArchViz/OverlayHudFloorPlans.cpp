#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/BuildingTopology.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <climits>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::overlayhud {
namespace {
namespace fs = floorscheme;
namespace bt = buildingtopology;
constexpr uint32_t kPlanFillAlpha = 0x99;  // a cell's fill on the plan: its colour at 60 %
constexpr double kEdgeLabelShortest = 0.6; // m: a side shorter carries no length
// The cell complex as its inspector draws it (user, 2026-10-15: "Show Cell Complex ... cells as in
// Cell Complex Inspector"): a space's colour as an architect's plan gives it, an ink edge at 35 %,
// and the inspector's accent round a highlighted stair.
constexpr uint32_t kCellEdge = 0x1D242759u, kCoreHighlight = 0x2448C9FFu;
uint32_t CellRgba (const bt::Cell& cell)
{
    switch (cell.kind) {
        case bt::CellKind::Room:
            switch (cell.room) {
                case fs::RoomKind::Living:
                    return 0xD9A441FFu;
                case fs::RoomKind::Bedroom:
                    return 0x7FA7C7FFu;
                case fs::RoomKind::Alcove:
                    return 0xA3BFD8FFu;
                case fs::RoomKind::Hall:
                    return 0xC5BFB2FFu;
                case fs::RoomKind::Bath:
                    return 0x5FAE9FFFu;
                case fs::RoomKind::Wc:
                    return 0x8CC4B9FFu;
                case fs::RoomKind::Storage:
                    return 0xA99A8BFFu;
            }
            return 0xD5CDC0FFu;
        case bt::CellKind::FlatRest:
            return 0xD5CDC0FFu;
        case bt::CellKind::Flat:
            return 0xCF9F98FFu;
        case bt::CellKind::Corridor:
            return 0xEAD9A6FFu;
        case bt::CellKind::Lobby:
            return 0xE7C97CFFu;
        case bt::CellKind::Core:
            return 0x8E3B2FFFu;
        case bt::CellKind::Unassigned:
            return 0xB9BEC1FFu;
    }
    return 0xB9BEC1FFu;
}

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
// A counter-clockwise ring raised from z0 to z1 into `mesh`, coloured `rgba`: its caps
// triangulated over its own corners (so their seams are no edge), its sides quads. Each cap and the
// sides have their own corners, so the mesh's feature edges are the solid's outline.
void Solid (overlaylayers::Mesh& mesh, const fs::Ring& ring, double z0, double z1, uint32_t rgba)
{
    namespace cp = Clipper2Lib;
    const uint32_t n = static_cast<uint32_t> (ring.size ());
    if (n < 3 || z1 <= z0)
        return;
    cp::PathD path;
    for (const auto& p : ring)
        path.emplace_back (p.x, p.y);
    cp::PathsD triangles;
    if (cp::Triangulate ({ path }, 4, triangles, false) != cp::TriangulateResult::success)
        return;
    const auto corner = [&] (const cp::PointD& q) {
        uint32_t best = 0;
        double nearest = 1e300;
        for (uint32_t i = 0; i < n; ++i)
            if (const double d = std::hypot (ring[i].x - q.x, ring[i].y - q.y); d < nearest)
                nearest = d, best = i;
        return best;
    };
    const auto put = [&] (double z) {
        const uint32_t base = static_cast<uint32_t> (mesh.points.size () / 3);
        for (const auto& p : ring)
            mesh.points.insert (mesh.points.end (), { p.x, p.y, z });
        mesh.vertexRgba.insert (mesh.vertexRgba.end (), n, rgba);
        return base;
    };
    const uint32_t bottom = put (z0), top = put (z1);
    for (const auto& t : triangles) {
        if (t.size () != 3)
            continue;
        uint32_t a = corner (t[0]), b = corner (t[1]), c = corner (t[2]);
        if ((t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x) < 0)
            std::swap (b, c); // counter-clockwise from above
        mesh.indices.insert (mesh.indices.end (), { top + a, top + b, top + c, bottom + a, bottom + c, bottom + b });
    }
    const uint32_t low = put (z0), high = put (z1);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = (i + 1) % n;
        mesh.indices.insert (mesh.indices.end (), { low + i, low + k, high + k, low + i, high + k, high + i });
    }
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

// The floors' schemes on the overlay as their cell complex (BuildingTopology.hpp; user, 2026-10-15):
// each cell a solid from its floor to under the next slab, coloured by its space, the stack's stair
// cells highlighted on their own toggle, and the flat selected in a Plan view white inside its walls
// with its floor's side lengths.
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
    const bool wanted = state.previewStairs || state.previewCells;
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
    signature << std::setprecision (17) << state.previewStairs << state.previewCells << ':'
              << state.floorPlanner.Revision () << ':' << (onPlan ? state.planStorey : INT_MIN);
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
        units.views = stairs.views = overlaylayers::Views::ThreeD;
    }
    // The cell complex (user, 2026-10-15: "Replace Show units and Show main walls with Show Cell
    // Complex"): in 3D every floor's cells, solids from the floor to under the next slab; on the plan
    // its storey's, filled. The stairs' toggle highlights the stack's stair cells, alone or among
    // the rest ("Show proposed stair cores becomes only higlight stair core cell").
    // Built again only when what they are built from changed: the planner's results or what it was
    // asked (user, 2026-10-16: a selection rebuilt every building's complex, 30-50 ms on the UI thread).
    const std::string from = std::to_string (state.floorPlanner.Revision ()) + '|' + asked.str ();
    if (from != state.floorComplexesFrom) {
        state.floorComplexes.clear ();
        state.floorComplexesFrom = from;
    }
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        if (!state.previewCells && !state.previewStairs)
            break;
        auto& held = state.floorComplexes[key];
        if (!held)
            held = std::make_shared<const bt::Complex> (ComplexOf (state, key, plan, options));
        const auto& complex = *held;
        overlaylayers::Mesh cells, cores;
        for (auto* mesh : { &cells, &cores }) {
            mesh->styled = true;
            mesh->style.shading = overlaylayers::Shading::Lit;
            mesh->style.behind = overlaylayers::Behind::Show;
            mesh->style.edgeRgba = kCellEdge;
        }
        cores.style.edgeRgba = kCoreHighlight;
        cores.style.edgeWidthPixels = 2.5f;
        for (const auto& cell : complex.cells) {
            if (cell.context || cell.floor < 0)
                continue;
            const bool core = cell.kind == bt::CellKind::Core;
            const bool highlighted = core && state.previewStairs;
            if (!highlighted && !state.previewCells)
                continue;
            const auto& floor = complex.floors[cell.floor];
            const auto span = bt::ClearSpan (floor);
            const uint32_t rgba = CellRgba (cell);
            Solid (highlighted ? cores : cells, cell.shape, span.z0 + 0.02, span.z1, rgba);
            if (onPlan && floor.story == state.planStorey) {
                const double z = floor.z + 0.02;
                Fill (*onPlan, cell.shape, z, (rgba & 0xFFFFFF00u) | (highlighted ? 0xE6u : kPlanFillAlpha));
                Line (*onPlan, cell.shape, z, highlighted ? kCoreHighlight : (kCellEdge & 0xFFFFFF00u) | 0x99u,
                      highlighted ? 3.0f : 1.0f);
            }
        }
        if (!cells.indices.empty ())
            units.meshes.push_back (std::move (cells));
        if (!cores.indices.empty ())
            stairs.meshes.push_back (std::move (cores));
    }
    // The flat selected in a Plan view, white over the rest, whether the cells are shown or not:
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
