#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/BuildingTopology.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include "ArchViz/MassingSlices.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <climits>
#include <cmath>
#include <array>
#include <map>
#include <cstdio>
#include <memory>
#include <sstream>
#include <iomanip>

namespace geomsrv::archviz::overlayhud {
namespace {
namespace fs = floorscheme;
namespace fp = floorprogramme;
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

// The presets' colours (CellColours): a palette for what has no colour of its own, circulation
// and stairs neutral beside the flats, and the floors' slabs as a wireframe around them (user,
// 2026-10-16: "Preview should be similar to ... Wireframe of main slab visible").
constexpr uint32_t kPalette[] = { 0xE8A3B4FFu, 0xA8C58AFFu, 0x5EC2E0FFu, 0xBEA74AFFu, 0xD4836EFFu, 0xC6A8E8FFu,
                                  0xF0C36AFFu, 0x7FB8A8FFu, 0x9AA7D9FFu, 0xD9A06BFFu, 0x8FD0C1FFu, 0xE5B7D8FFu };
constexpr uint32_t kNeutral = 0xE4E6E6FFu, kCoreNeutral = 0xB8BCBEFFu, kNoProgramme = 0xA9BCD0FFu;
constexpr uint32_t kFaded = 0xF3F4F4FFu; // what a Stats highlight leaves out
constexpr uint32_t kSlabWire = 0x3C4248B0u, kUnitInk = 0x1D2427CCu, kInnerWall = 0x1D24276Eu;
uint32_t Paletted (size_t index)
{
    return kPalette[index % (sizeof (kPalette) / sizeof (kPalette[0]))];
}
double SignedArea (const fs::Ring& ring)
{
    double a = 0;
    for (size_t i = 0; i < ring.size (); ++i)
        a += ring[i].x * ring[(i + 1) % ring.size ()].y - ring[(i + 1) % ring.size ()].x * ring[i].y;
    return a / 2;
}
// A flat's look for "Unique units": its room count, and its area inside its walls and its sides'
// lengths to 0.1 m, longest first -- the same flat turned or mirrored is the same.
std::string Signature (const bt::Unit& unit, const std::vector<fs::Ring>& rings)
{
    double area = 0;
    std::vector<long long> sides;
    for (const auto& ring : rings) {
        area += std::abs (SignedArea (ring));
        for (size_t i = 0; i < ring.size (); ++i)
            sides.push_back (std::llround (
                std::hypot (ring[(i + 1) % ring.size ()].x - ring[i].x, ring[(i + 1) % ring.size ()].y - ring[i].y) *
                10));
    }
    std::sort (sides.rbegin (), sides.rend ());
    std::string out = std::to_string (std::llround (unit.rooms * 2)) + ':' + std::to_string (std::llround (area * 10));
    for (long long side : sides)
        out += ',' + std::to_string (side);
    return out;
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

// What the overlay draws a building from, kept while it stands (State::floorComplexes): its cell
// complex, and each flat and each cell inside its walls -- clipping them each time the selection or
// the preset changed cost 10-50 ms (user, 2026-10-16: "navigation stutters").
struct FloorCells {
    std::string from; // what it was built from: the building's floors' planned schemes and what was asked
    bt::Complex complex;
    std::vector<std::vector<fs::Ring>> units, cells;
};

namespace {
// A point inside a ring: its first triangle's centre (a flat's centre may be outside an L).
fs::Vec Inside (const fs::Ring& ring)
{
    namespace cp = Clipper2Lib;
    cp::PathD path;
    for (const auto& p : ring)
        path.emplace_back (p.x, p.y);
    cp::PathsD triangles;
    if (ring.size () >= 3 && cp::Triangulate ({ path }, 4, triangles, false) == cp::TriangulateResult::success &&
        !triangles.empty () && triangles.front ().size () == 3) {
        const auto& t = triangles.front ();
        return { (t[0].x + t[1].x + t[2].x) / 3, (t[0].y + t[1].y + t[2].y) / 3 };
    }
    return ring.empty () ? fs::Vec {} : ring.front ();
}
// Whether view pixel (x, y) is inside the projected polygon `screen` (even-odd).
bool Covers (const std::vector<std::array<float, 2>>& screen, float x, float y)
{
    bool in = false;
    for (size_t i = 0, j = screen.size () - 1; i < screen.size (); j = i++)
        if ((screen[i][1] > y) != (screen[j][1] > y) &&
            x < (screen[j][0] - screen[i][0]) * (y - screen[i][1]) / (screen[j][1] - screen[i][1]) + screen[i][0])
            in = !in;
    return in;
}
} // namespace

std::optional<hudfloorscheme::Hit> PickCell (const State& state, const Input& input, float x, float y)
{
    if (!input.project || !input.hasCameraDirection || !state.previewCells)
        return std::nullopt;
    const double* d = input.cameraDirection;
    // The nearest face under the pointer of every solid -- the flats', and the cells no flat holds,
    // which hide what is behind them -- its depth the mean of its corners' along the view.
    double nearest = 1e300;
    std::optional<hudfloorscheme::Hit> found;
    const auto test = [&] (const fs::Ring& ring, double z0, double z1, const std::string& key, int story, bool flat) {
        const size_t n = ring.size ();
        if (n < 3)
            return;
        std::vector<std::array<float, 2>> low (n), high (n);
        for (size_t i = 0; i < n; ++i)
            if (!input.project (ring[i].x, ring[i].y, z0, low[i][0], low[i][1]) ||
                !input.project (ring[i].x, ring[i].y, z1, high[i][0], high[i][1]))
                return; // a corner behind the eye: not pickable from here
        double at = 1e300;
        const auto depth = [&] (double px, double py, double pz) { return px * d[0] + py * d[1] + pz * d[2]; };
        double mean0 = 0, mean1 = 0;
        for (const auto& p : ring)
            mean0 += depth (p.x, p.y, z0) / n, mean1 += depth (p.x, p.y, z1) / n;
        if (Covers (low, x, y))
            at = (std::min) (at, mean0);
        if (Covers (high, x, y))
            at = (std::min) (at, mean1);
        for (size_t i = 0; i < n; ++i) {
            const size_t k = (i + 1) % n;
            if (Covers ({ low[i], low[k], high[k], high[i] }, x, y))
                at = (std::min) (at, (depth (ring[i].x, ring[i].y, z0) + depth (ring[k].x, ring[k].y, z0) +
                                      depth (ring[i].x, ring[i].y, z1) + depth (ring[k].x, ring[k].y, z1)) /
                                         4);
        }
        if (at >= nearest)
            return;
        nearest = at;
        found = flat ? std::optional<hudfloorscheme::Hit> (hudfloorscheme::Hit { key, story, Inside (ring) })
                     : std::nullopt;
    };
    for (const auto& [key, held] : state.floorComplexes) {
        if (!held)
            continue;
        const auto& complex = held->complex;
        for (size_t u = 0; u < complex.units.size (); ++u) {
            const auto& floor = complex.floors[complex.units[u].floor];
            const auto span = bt::ClearSpan (floor);
            for (const auto& ring : held->units[u])
                test (ring, span.z0 + 0.02, span.z1, key, floor.story, true);
        }
        for (size_t id = 0; id < complex.cells.size (); ++id) {
            const auto& cell = complex.cells[id];
            if (cell.context || cell.floor < 0 || cell.unit >= 0)
                continue;
            const auto span = bt::ClearSpan (complex.floors[cell.floor]);
            for (const auto& ring : held->cells[id])
                test (ring, span.z0 + 0.02, span.z1, key, complex.floors[cell.floor].story, false);
        }
    }
    return found;
}

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
    signature << std::setprecision (17) << state.previewStairs << state.previewCells << int (state.cellColours)
              << state.cellHiddenLines << state.massingStatsFunction << ':' << state.floorPlanner.Revision () << ':'
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
        units.views = stairs.views = overlaylayers::Views::ThreeD;
    }
    // The cell complex (user, 2026-10-15: "Replace Show units and Show main walls with Show Cell
    // Complex"): in 3D every floor, each flat a solid inside its walls from the floor to under the next
    // slab, circulation and stairs beside them, the floors' slabs a wireframe round them; on the plan
    // its storey's, each flat one colour with its inner walls thin (user, 2026-10-16: "in floor plan
    // view show unit as one color and inner walls thin lines similar like in mini GUI"). Coloured by
    // the Massing tab's preset. The stairs' toggle highlights the stack's stair cells, alone or among
    // the rest. Solid layers: their solids hide each other, hidden edges dashed; the model hides
    // them but for their own slabs, and a slice in front of them is drawn over them (Layer::solid).
    units.solid = stairs.solid = true;
    // Each building's built again only when what it is built from changed: its floors' planned
    // schemes, or what the planner was asked (user, 2026-10-16: a selection rebuilt every building's
    // complex, 30-50 ms on the UI thread; a scheme arriving for one floor rebuilt them all).
    if (asked.str () != state.floorComplexesFrom) {
        state.floorComplexes.clear ();
        state.floorComplexesFrom = asked.str ();
    }
    std::erase_if (state.floorComplexes,
                   [&] (const auto& held) { return !state.floorPlanSnapshots.contains (held.first); });
    const bool drawn = state.previewCells || state.previewStairs;
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        std::string from;
        for (const auto& floor : plan.floors)
            if (const auto* planned = state.floorPlanner.Latest (FloorId (key, floor.story)))
                from += ',' + std::to_string (planned->revision);
            else
                from += ",-";
        if (const auto* stack = state.floorPlanner.Latest (StackId (key, plan)))
            from += '#' + std::to_string (stack->revision);
        auto& held = state.floorComplexes[key];
        if (held && held->from != from)
            held.reset ();
        if (drawn && !held) {
            auto built = std::make_shared<FloorCells> ();
            built->from = from;
            built->complex = ComplexOf (state, key, plan, options);
            for (const auto& unit : built->complex.units)
                built->units.push_back (bt::Clear (built->complex, unit.cells));
            for (size_t id = 0; id < built->complex.cells.size (); ++id)
                built->cells.push_back (built->complex.cells[id].context ? std::vector<fs::Ring> {}
                                                                         : bt::Clear (built->complex, { int (id) }));
            held = std::move (built);
        }
    }
    // Each flat inside its walls, once; and what "Unique units" and "Story heights" key their
    // colours by, over every building, in a stable order.
    std::map<std::string, size_t> looks;
    std::map<long long, size_t> heights;
    for (const auto& [key, held] : state.floorComplexes) {
        if (!held || !state.previewCells)
            continue;
        for (size_t u = 0; u < held->units.size (); ++u)
            looks.emplace (Signature (held->complex.units[u], held->units[u]), 0);
        for (const auto& floor : held->complex.floors)
            heights.emplace (std::llround (floor.height * 100), 0);
    }
    size_t next = 0;
    for (auto& [look, index] : looks)
        index = next++;
    next = 0;
    for (auto& [height, index] : heights)
        index = next++;
    size_t building = 0;
    for (const auto& [key, plan] : state.floorPlanSnapshots) {
        const size_t index = building++;
        const auto found = state.floorComplexes.find (key);
        if (!drawn || found == state.floorComplexes.end () || !found->second)
            continue;
        const auto& complex = found->second->complex;
        const auto& rings = found->second->units;
        const auto& clear = found->second->cells;
        // Its cells stand inside its own slabs: those alone hide none of them (Layer::inside).
        for (auto* layer : { &units, &stairs })
            layer->inside.insert (layer->inside.end (), plan.guids.begin (), plan.guids.end ());
        const auto programmeOf = [&] (int story) {
            for (const auto& floor : plan.floors)
                if (floor.story == story && (floor.programmeRgba & 0xFFu) != 0)
                    return floor.programmeRgba | 0xFFu;
            return kNoProgramme;
        };
        // ⚠️ THE STATS TAB'S HIGHLIGHT ON THE CELLS (user, 2026-10-16: "highlights from stats tab
        // should work with cell complex. Cell complex should be first class preview"): the solids are
        // drawn after the function volumes and hide them, so the cells answer the hover themselves --
        // a use: its floors in its colour, the rest faded; the built footprint: each building's ground
        // floor in the built colour. The unbuilt parcel is outside the buildings and stays as it is.
        const std::string& hover = state.massingStatsFunction;
        const bool built = hover == massingslices::kBuiltHover;
        const bool use = !hover.empty () && !built && hover != massingslices::kUnbuiltHover;
        double ground = 1e300;
        for (const auto& floor : complex.floors)
            ground = (std::min) (ground, floor.z);
        const auto hovered = [&] (const bt::Floor& floor, uint32_t rgba) -> uint32_t {
            if (built)
                return std::abs (floor.z - ground) < 1e-6
                           ? graphicssettings::Colour ("ui.coverage.built", 0x9AA0A6FFu) | 0xFFu
                           : kFaded;
            if (!use)
                return rgba;
            for (const auto& f : plan.floors)
                if (f.story == floor.story && f.programme == hover)
                    return graphicssettings::FunctionColour (hover, f.programmeRgba) | 0xFFu;
            return kFaded;
        };
        // What a flat, or a cell no flat holds, is coloured on this floor.
        const auto ownColour = [&] (const bt::Floor& floor, const bt::Unit* unit, const std::vector<fs::Ring>* rings,
                                    const bt::Cell* cell) -> uint32_t {
            if (state.cellColours == CellColours::SpaceTypes && cell)
                return CellRgba (*cell);
            if (!unit)
                return cell && cell->kind == bt::CellKind::Core ? kCoreNeutral : kNeutral;
            switch (state.cellColours) {
                case CellColours::Programs:
                    return programmeOf (floor.story);
                case CellColours::UniqueUnits:
                    return Paletted (looks[Signature (*unit, *rings)]);
                case CellColours::StoryHeights:
                    return Paletted (heights[std::llround (floor.height * 100)]);
                case CellColours::Buildings:
                    return Paletted (index);
                default:
                    return floorprogramme::Colour (state.massingProgramme, unit->type, unit->rooms) | 0xFFu;
            }
        };
        const auto colourOf = [&] (const bt::Floor& floor, const bt::Unit* unit, const std::vector<fs::Ring>* rings,
                                   const bt::Cell* cell) -> uint32_t {
            return hovered (floor, ownColour (floor, unit, rings, cell));
        };
        overlaylayers::Mesh cells, cores;
        for (auto* mesh : { &cells, &cores }) {
            mesh->styled = true;
            mesh->style.shading = overlaylayers::Shading::Lit;
            mesh->style.behind = overlaylayers::Behind::Show;
            mesh->style.edgeRgba = kCellEdge;
        }
        cores.style.edgeRgba = kCoreHighlight;
        cores.style.edgeWidthPixels = 2.5f;
        // The flat selected: its solid lit and edged in the accent (user, 2026-10-16: "In locked mode 3d
        // view selection should select cell and higlight it, not the 2d contour"); its rooms too.
        overlaylayers::Mesh chosen = cores;
        chosen.style.edgeWidthPixels = 3.0f;
        std::vector<char> picked (complex.units.size (), 0);
        for (const auto& sel : selected) {
            if (sel.building != key)
                continue;
            const auto* planned = state.floorPlanner.Latest (FloorId (key, sel.story));
            for (size_t u = 0; planned && u < complex.units.size (); ++u) {
                const auto& unit = complex.units[u];
                if (complex.floors[unit.floor].story == sel.story && unit.flat >= 0 &&
                    unit.flat < int (planned->scheme.flats.size ()) &&
                    planned->scheme.flats[unit.flat].shape == sel.flat)
                    picked[u] = 1;
            }
        }
        const auto lit = [] (uint32_t rgba) {
            const auto mix = [&] (int shift) {
                const uint32_t c = (rgba >> shift) & 0xFFu;
                return (c + (255 - c) * 11 / 20) << shift;
            };
            return mix (24) | mix (16) | mix (8) | 0xFFu;
        };
        // Behind other cells, the edges dashed and faint -- or not drawn (the Massing tab's switch).
        const auto behind = state.cellHiddenLines ? overlaylayers::Behind::Show : overlaylayers::Behind::Hide;
        for (auto* mesh : { &cells, &cores, &chosen })
            mesh->style.behind = behind;
        // The flats: one solid each, or a solid a room under "Space types"; on the plan one fill,
        // its outline and its inner walls thin.
        for (size_t u = 0; u < complex.units.size () && state.previewCells; ++u) {
            const auto& unit = complex.units[u];
            const auto& floor = complex.floors[unit.floor];
            const auto span = bt::ClearSpan (floor);
            overlaylayers::Layer* here = onPlan && floor.story == state.planStorey ? onPlan : nullptr;
            const double z = floor.z + 0.02;
            auto& into = picked[u] ? chosen : cells;
            if (state.cellColours == CellColours::SpaceTypes)
                for (int id : unit.cells) {
                    const uint32_t own = hovered (floor, CellRgba (complex.cells[id])),
                                   rgba = picked[u] ? lit (own) : own;
                    for (const auto& ring : clear[id]) {
                        Solid (into, ring, span.z0 + 0.02, span.z1, rgba);
                        if (here)
                            Fill (*here, ring, z, (rgba & 0xFFFFFF00u) | kPlanFillAlpha);
                    }
                }
            else {
                const uint32_t own = colourOf (floor, &unit, &rings[u], nullptr), rgba = picked[u] ? lit (own) : own;
                for (const auto& ring : rings[u]) {
                    Solid (into, ring, span.z0 + 0.02, span.z1, rgba);
                    if (here)
                        Fill (*here, ring, z, (rgba & 0xFFFFFF00u) | kPlanFillAlpha);
                }
            }
            if (!here)
                continue;
            for (const auto& ring : rings[u])
                Line (*here, ring, z, kUnitInk, 1.3f);
            // Its inner walls: the faces between two of its own rooms, on their centre lines.
            for (int id : unit.cells)
                for (int f : complex.cells[id].faces) {
                    const auto& face = complex.faces[f];
                    if (face.horizontal || face.kind != bt::FaceClass::Internal || face.cells[0] != id)
                        continue;
                    overlaylayers::Polyline wall;
                    wall.rgba = kInnerWall;
                    wall.widthPixels = 1.0f;
                    wall.behind = overlaylayers::Behind::Show;
                    wall.points = { face.a.x, face.a.y, z, face.b.x, face.b.y, z };
                    here->polylines.push_back (std::move (wall));
                }
        }
        // Circulation, the stairs and floor no flat takes.
        for (size_t id = 0; id < complex.cells.size (); ++id) {
            const auto& cell = complex.cells[id];
            if (cell.context || cell.floor < 0 || cell.unit >= 0)
                continue;
            const bool highlighted = cell.kind == bt::CellKind::Core && state.previewStairs;
            if (!highlighted && !state.previewCells)
                continue;
            const auto& floor = complex.floors[cell.floor];
            const auto span = bt::ClearSpan (floor);
            const uint32_t rgba = highlighted ? CellRgba (cell) : colourOf (floor, nullptr, nullptr, &cell);
            const bool here = onPlan && floor.story == state.planStorey;
            for (const auto& ring : clear[id]) {
                Solid (highlighted ? cores : cells, ring, span.z0 + 0.02, span.z1, rgba);
                if (!here)
                    continue;
                const double z = floor.z + 0.02;
                Fill (*onPlan, ring, z, (rgba & 0xFFFFFF00u) | (highlighted ? 0xE6u : kPlanFillAlpha));
                Line (*onPlan, ring, z, highlighted ? kCoreHighlight : kUnitInk, highlighted ? 3.0f : 1.0f);
            }
        }
        // The floors' slabs: each floor's outline at its level and its top, and its corners.
        for (const auto& floor : complex.floors) {
            if (!state.previewCells)
                break;
            for (const auto& ring : floor.outline) {
                for (const double z : { floor.z, floor.z + floor.height })
                    Line (units, ring, z, kSlabWire, 1.0f);
                for (auto it = units.polylines.end () - 2; it != units.polylines.end (); ++it)
                    it->behind = behind;
                for (const auto& p : ring) {
                    overlaylayers::Polyline edge;
                    edge.rgba = kSlabWire;
                    edge.widthPixels = 1.0f;
                    edge.behind = behind;
                    edge.points = { p.x, p.y, floor.z, p.x, p.y, floor.z + floor.height };
                    units.polylines.push_back (std::move (edge));
                }
            }
        }
        if (!cells.indices.empty ())
            units.meshes.push_back (std::move (cells));
        if (!cores.indices.empty ())
            stairs.meshes.push_back (std::move (cores));
        if (!chosen.indices.empty ())
            units.meshes.push_back (std::move (chosen));
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
            // In 3D on its solid's top, where the solids leave it seen (Layer::solid).
            const double top = floor.z + floor.height - fp::kSlab + 0.02;
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
                if (!state.previewCells)
                    Line (units, ring, top, 0xFFFFFFFFu, 5.0f);
                if (here)
                    Line (*here, ring, floor.z + 0.04, 0xFFFFFFFFu, 5.0f);
            }
            for (const auto& ring : measured) {
                EdgeLengths (units, ring, top + 0.01);
                if (here)
                    EdgeLengths (*here, ring, floor.z + 0.05);
            }
        }
    }
    return true;
}
} // namespace geomsrv::archviz::overlayhud
