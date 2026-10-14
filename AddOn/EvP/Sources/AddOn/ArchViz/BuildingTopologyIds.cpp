#include "ArchViz/BuildingTopologyDetail.hpp"

// Ids that outlive a rebuild. Cells and faces are rebuilt from the schemes every time, so a flat
// or a room is found again by what it is: a space of the same kind on the same story, covering
// most of the same floor. Matching is greedy on the share of joint area, largest first, so the
// result does not depend on cell order beyond ties. Faces keep no id: they are rebuilt.
namespace geomsrv::archviz::buildingtopology {
using namespace detail;

namespace {
constexpr double kLineage = 0.25;   // of either area: an overlap that names a predecessor
constexpr double kMinLineage = 0.5; // m2: a smaller overlap names nothing

// Spaces that can be one another across a rebuild.
int Family (CellKind kind)
{
    switch (kind) {
        case CellKind::Room:
        case CellKind::FlatRest:
        case CellKind::Flat:
            return 0;
        case CellKind::Corridor:
        case CellKind::Lobby:
            return 1;
        case CellKind::Core:
            return 2;
        case CellKind::Unassigned:
            return 3;
    }
    return 4;
}
struct Space {
    cp::PathsD paths;
    double area = 0, z = 0;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    int story = 0, family = 0;
    bool skip = false; // context: no id
};
Space Make (const std::vector<Ring>& rings, const Floor& floor, int family)
{
    Space s;
    s.paths = ToPaths (rings);
    s.area = std::abs (Area (s.paths));
    s.story = floor.story, s.z = floor.z, s.family = family;
    for (const auto& ring : rings)
        for (const auto& p : ring)
            s.x0 = (std::min) (s.x0, p.x), s.y0 = (std::min) (s.y0, p.y), s.x1 = (std::max) (s.x1, p.x),
            s.y1 = (std::max) (s.y1, p.y);
    return s;
}
std::vector<Space> Units (const Complex& complex)
{
    std::vector<Space> out;
    for (const auto& unit : complex.units)
        out.push_back (Make (Union (complex, unit.cells), complex.floors[unit.floor], 0));
    return out;
}
std::vector<Space> Cells (const Complex& complex)
{
    std::vector<Space> out;
    for (const auto& cell : complex.cells) {
        out.push_back (Make ({ cell.shape }, complex.floors[cell.floor], Family (cell.kind)));
        out.back ().skip = cell.context;
    }
    return out;
}
// For each of `after`, the one of `before` it is (-1: none) and, for the others, what it came from.
std::vector<int> Match (const std::vector<Space>& before, const std::vector<Space>& after,
                        std::vector<std::vector<int>>& lineage)
{
    struct Pair {
        double share, common;
        int a, b;
    };
    std::vector<Pair> pairs;
    for (size_t a = 0; a < after.size (); ++a)
        for (size_t b = 0; b < before.size (); ++b) {
            const auto &p = after[a], &q = before[b];
            if (p.skip || q.skip || p.story != q.story || p.family != q.family || std::abs (p.z - q.z) > kStacked ||
                p.x0 >= q.x1 || q.x0 >= p.x1 || p.y0 >= q.y1 || q.y0 >= p.y1)
                continue;
            const double common = std::abs (Area (cp::Intersect (p.paths, q.paths, cp::FillRule::NonZero, kPrecision)));
            if (common > 1e-6)
                pairs.push_back (
                    { common / (p.area + q.area - common), common, static_cast<int> (a), static_cast<int> (b) });
        }
    std::sort (pairs.begin (), pairs.end (), [] (const Pair& x, const Pair& y) {
        return x.share != y.share ? x.share > y.share : x.a != y.a ? x.a < y.a : x.b < y.b;
    });
    std::vector<int> matched (after.size (), -1);
    std::vector<char> used (before.size (), 0);
    for (const auto& p : pairs)
        if (p.share >= kSame && matched[p.a] < 0 && !used[p.b])
            matched[p.a] = p.b, used[p.b] = 1;
    lineage.assign (after.size (), {});
    for (const auto& p : pairs)
        if (matched[p.a] < 0 && p.common >= kMinLineage &&
            (p.common >= kLineage * before[p.b].area || p.common >= kLineage * after[p.a].area))
            lineage[p.a].push_back (p.b);
    for (auto& list : lineage)
        std::sort (list.begin (), list.end ());
    return matched;
}
// Ids from `before`'s matches, new ids from `next` in order for the rest.
template <typename Item>
void Assign (const std::vector<Item>& before, std::vector<Item>& after, const std::vector<Space>& spaces,
             const std::vector<int>& matched, const std::vector<std::vector<int>>& lineage, uint64_t& next)
{
    for (size_t i = 0; i < after.size (); ++i) {
        auto& item = after[i];
        item.derivedFrom.clear ();
        if (spaces[i].skip) {
            item.id = 0;
            continue;
        }
        if (matched[i] >= 0 && before[matched[i]].id != 0) {
            item.id = before[matched[i]].id;
            continue;
        }
        item.id = next++;
        for (int b : lineage[i])
            if (before[b].id != 0)
                item.derivedFrom.push_back (before[b].id);
    }
}
} // namespace

void Reconcile (const Complex& before, Complex& after, uint64_t& next)
{
    if (next == 0)
        next = 1; // 0 is no id
    std::vector<std::vector<int>> lineage;
    const auto unitsAfter = Units (after);
    const auto units = Match (Units (before), unitsAfter, lineage);
    Assign (before.units, after.units, unitsAfter, units, lineage, next);
    const auto cellsAfter = Cells (after);
    const auto cells = Match (Cells (before), cellsAfter, lineage);
    Assign (before.cells, after.cells, cellsAfter, cells, lineage, next);
}
int UnitById (const Complex& complex, uint64_t id)
{
    for (size_t i = 0; i < complex.units.size () && id != 0; ++i)
        if (complex.units[i].id == id)
            return static_cast<int> (i);
    return -1;
}
int CellById (const Complex& complex, uint64_t id)
{
    for (size_t i = 0; i < complex.cells.size () && id != 0; ++i)
        if (complex.cells[i].id == id)
            return static_cast<int> (i);
    return -1;
}
} // namespace geomsrv::archviz::buildingtopology
