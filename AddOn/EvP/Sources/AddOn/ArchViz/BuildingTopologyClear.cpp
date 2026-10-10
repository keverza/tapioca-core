#include "ArchViz/BuildingTopologyDetail.hpp"

// Clear geometry from cells and walls: every face that bounds a set of cells is a band of its
// cells' share of the wall (the whole facade thickness inward, half a partition each side), and
// the set less those bands is what lies inside the walls. Square band ends close the corner of
// two walls at a re-entrant corner.
namespace geomsrv::archviz::buildingtopology {
using namespace detail;

std::vector<Ring> Clear (const Complex& complex, const std::vector<int>& cells)
{
    std::vector<char> in (complex.cells.size (), 0);
    cp::PathsD set;
    for (int id : cells)
        if (id >= 0 && id < static_cast<int> (complex.cells.size ()) && !in[id]) {
            in[id] = 1;
            set.push_back (ToPath (complex.cells[id].shape));
        }
    if (set.empty ())
        return {};
    cp::PathsD bands;
    for (int id : cells) {
        if (id < 0 || id >= static_cast<int> (complex.cells.size ()))
            continue;
        for (int face : complex.cells[id].faces) {
            const auto& f = complex.faces[face];
            const int other = f.cells[0] == id ? f.cells[1] : f.cells[0];
            if (other >= 0 && in[other])
                continue; // between two cells of the set: not its wall
            const double share = Share (complex, face, id);
            if (share <= 0)
                continue;
            const auto band = cp::InflatePaths ({ cp::PathD { { f.a.x, f.a.y }, { f.b.x, f.b.y } } }, share,
                                                cp::JoinType::Miter, cp::EndType::Square, 2.0, kPrecision);
            bands.insert (bands.end (), band.begin (), band.end ());
        }
    }
    const auto whole = cp::Union (set, cp::FillRule::NonZero, kPrecision);
    const auto clear = bands.empty () ? whole
                                      : cp::Difference (whole, cp::Union (bands, cp::FillRule::NonZero, kPrecision),
                                                        cp::FillRule::NonZero, kPrecision);
    std::vector<Ring> out;
    for (const auto& path : clear)
        if (std::abs (cp::Area (path)) > 1e-4)
            out.push_back (FromPath (cp::SimplifyPath (path, 1e-4)));
    return out;
}
std::vector<Ring> Plate (const Complex& complex, int floor)
{
    if (floor < 0 || floor >= static_cast<int> (complex.floors.size ()))
        return {};
    return Clear (complex, complex.floors[floor].cells);
}
Span SlabSpan (const Floor& floor)
{
    return { floor.z - floorprogramme::kSlab, floor.z };
}
Span ClearSpan (const Floor& floor)
{
    return { floor.z, floor.z + (std::max) (0.0, floor.height - floorprogramme::kSlab) };
}
} // namespace geomsrv::archviz::buildingtopology
