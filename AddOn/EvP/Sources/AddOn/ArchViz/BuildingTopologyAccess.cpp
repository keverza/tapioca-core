#include "ArchViz/BuildingTopologyDetail.hpp"
#include <deque>
#include <map>
#include <set>

// Adjacency, access and exterior contact over the cells, and the rules that read them. Every link
// comes from a face (or, between floors, from the building's stair stack), so the graphs change
// only when the cells and walls do.
namespace geomsrv::archviz::buildingtopology {
using namespace detail;
namespace fs = floorscheme;

namespace {
double Length (const Face& f)
{
    return std::hypot (f.b.x - f.a.x, f.b.y - f.a.y);
}
bool Circulation (CellKind k)
{
    return k == CellKind::Corridor || k == CellKind::Lobby;
}
void Add (Graph& g, Link link)
{
    const int id = static_cast<int> (g.links.size ());
    if (link.from >= 0)
        g.at[link.from].push_back (id);
    if (link.to >= 0)
        g.at[link.to].push_back (id);
    g.links.push_back (link);
}
Graph Empty (const Complex& complex)
{
    Graph g;
    g.at.resize (complex.cells.size ());
    return g;
}
} // namespace

Graph Adjacency (const Complex& complex)
{
    auto g = Empty (complex);
    for (size_t i = 0; i < complex.faces.size (); ++i) {
        const auto& f = complex.faces[i];
        if (f.beyond == Beyond::Cell && f.cells[1] >= 0 && Length (f) > kContact)
            Add (g, { f.cells[0], f.cells[1], static_cast<int> (i), -1, Length (f) });
    }
    return g;
}
Graph Access (const Complex& complex)
{
    auto g = Empty (complex);
    for (size_t i = 0; i < complex.faces.size (); ++i) {
        const auto& f = complex.faces[i];
        if (f.beyond != Beyond::Cell || f.cells[1] < 0)
            continue;
        const int face = static_cast<int> (i);
        const auto& a = complex.cells[f.cells[0]];
        const auto& b = complex.cells[f.cells[1]];
        int door = -1;
        for (int ap : f.apertures)
            if (complex.apertures[ap].kind == ApertureKind::Door)
                door = ap;
        const bool open = f.kind == FaceClass::Open;
        const bool landing =
            (a.kind == CellKind::Core && Circulation (b.kind)) || (b.kind == CellKind::Core && Circulation (a.kind));
        if (door >= 0 || open || landing)
            Add (g, { f.cells[0], f.cells[1], face, door, Length (f) });
    }
    // A stack stair to itself on the next floor up (floors are lowest first).
    std::map<int, std::map<int, int>> stairs; // stack stair: floor -> cell
    for (size_t i = 0; i < complex.cells.size (); ++i)
        if (complex.cells[i].kind == CellKind::Core && complex.cells[i].stack >= 0)
            stairs[complex.cells[i].stack][complex.cells[i].floor] = static_cast<int> (i);
    for (const auto& [stair, floors] : stairs)
        for (auto it = floors.begin (); it != floors.end (); ++it) {
            const auto next = std::next (it);
            if (next != floors.end () && next->first == it->first + 1)
                Add (g, { it->second, next->second, -1, -1, 0 });
        }
    return g;
}
Graph Exterior (const Complex& complex)
{
    auto g = Empty (complex);
    for (size_t i = 0; i < complex.faces.size (); ++i) {
        const auto& f = complex.faces[i];
        if (f.beyond != Beyond::Outside && f.beyond != Beyond::Courtyard)
            continue;
        int window = -1;
        for (int ap : f.apertures)
            if (complex.apertures[ap].kind == ApertureKind::Window)
                window = ap;
        Add (g, { f.cells[0], -1, static_cast<int> (i), window, Length (f) });
    }
    return g;
}
std::vector<char> Reachable (const Complex& complex, const Graph& graph, const std::vector<int>& from)
{
    std::vector<char> seen (complex.cells.size (), 0);
    std::deque<int> queue;
    for (int id : from)
        if (id >= 0 && id < static_cast<int> (seen.size ()) && !seen[id])
            seen[id] = 1, queue.push_back (id);
    while (!queue.empty ()) {
        const int cell = queue.front ();
        queue.pop_front ();
        for (int l : graph.at[cell]) {
            const auto& link = graph.links[l];
            const int next = link.from == cell ? link.to : link.from;
            if (next >= 0 && !seen[next])
                seen[next] = 1, queue.push_back (next);
        }
    }
    return seen;
}

namespace {
struct Read {
    Graph access, exterior;
    std::vector<char> fromStairs;
};
Read ReadGraphs (const Complex& complex)
{
    Read r { Access (complex), Exterior (complex), {} };
    std::vector<int> stairs;
    for (size_t i = 0; i < complex.cells.size (); ++i)
        if (complex.cells[i].kind == CellKind::Core)
            stairs.push_back (static_cast<int> (i));
    r.fromStairs = Reachable (complex, r.access, stairs);
    return r;
}
UnitFigures FiguresOf (const Complex& complex, const Read& r, int unit)
{
    UnitFigures out;
    const auto& u = complex.units[unit];
    std::set<int> mine (u.cells.begin (), u.cells.end ());
    for (const auto& ring : Clear (complex, u.cells))
        out.clearArea += fs::Area (ring);
    for (int cell : u.cells)
        for (int face : complex.cells[cell].faces) {
            const auto& f = complex.faces[face];
            if (f.kind == FaceClass::Facade)
                out.frontage += Length (f);
            else if (f.kind == FaceClass::UnitParty)
                out.partyWall += Length (f);
            for (int ap : f.apertures) {
                const auto& a = complex.apertures[ap];
                if (a.kind == ApertureKind::Window && mine.count (a.cell))
                    ++out.windows;
                if (a.kind == ApertureKind::Door && mine.count (a.cell)) {
                    out.entrance = true;
                    out.reachesStair = out.reachesStair || (!r.fromStairs.empty () && r.fromStairs[a.cell]);
                }
            }
        }
    return out;
}
} // namespace

UnitFigures Figures (const Complex& complex, int unit)
{
    if (unit < 0 || unit >= static_cast<int> (complex.units.size ()))
        return {};
    return FiguresOf (complex, ReadGraphs (complex), unit);
}

std::vector<Diagnostic> Check (const Complex& complex)
{
    std::vector<Diagnostic> out;
    const auto r = ReadGraphs (complex);
    auto at = [&] (int cell) { return complex.cells[cell].shape.empty () ? Vec {} : complex.cells[cell].shape[0]; };
    for (size_t i = 0; i < complex.units.size (); ++i) {
        const auto& u = complex.units[i];
        if (u.cells.empty ())
            continue;
        const auto figures = FiguresOf (complex, r, static_cast<int> (i));
        if (!figures.entrance)
            out.push_back ({ "unit.no_entrance", "A flat with no door onto circulation or a stair.", u.floor,
                             at (u.cells.front ()) });
        else if (!figures.reachesStair)
            out.push_back ({ "unit.unreachable", "A flat whose door reaches no stair of the building.", u.floor,
                             at (u.cells.front ()) });
    }
    // Living rooms, bedrooms and alcoves need open air or a courtyard beyond a wall.
    for (size_t i = 0; i < complex.cells.size (); ++i) {
        const auto& cell = complex.cells[i];
        const bool habitable =
            cell.kind == CellKind::Room && (cell.room == fs::RoomKind::Living || cell.room == fs::RoomKind::Bedroom ||
                                            cell.room == fs::RoomKind::Alcove);
        if (habitable && r.exterior.at[i].empty ())
            out.push_back ({ "room.no_daylight", "A living room, bedroom or alcove with no facade.", cell.floor,
                             at (static_cast<int> (i)) });
    }
    // Every stack stair on every floor that has cells, from the lowest to the top.
    std::map<int, std::set<int>> stairs;
    for (const auto& cell : complex.cells)
        if (cell.kind == CellKind::Core && cell.stack >= 0)
            stairs[cell.stack].insert (cell.floor);
    for (const auto& [stair, floors] : stairs)
        for (size_t f = 0; f < complex.floors.size (); ++f)
            if (!complex.floors[f].cells.empty () && !floors.count (static_cast<int> (f)))
                out.push_back ({ "core.broken_stack",
                                 "A stair of the building is missing on this floor.",
                                 static_cast<int> (f),
                                 {} });
    return out;
}
} // namespace geomsrv::archviz::buildingtopology
