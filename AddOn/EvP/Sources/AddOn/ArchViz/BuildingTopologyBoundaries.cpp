#include "ArchViz/BuildingTopologyDetail.hpp"

// Faces of one floor. Every cell edge is matched against the collinear edges of the other cells:
// each overlap longer than kContact is one face with the two cells either side (split wherever
// a neighbour's edge starts or ends, so a long wall meeting two short ones gives two faces). What
// no other cell covers looks just beyond itself: open air, a courtyard, a neighbouring building,
// or floor no cell takes (a gap the scheme left). Then the generator's windows and doors are put
// on the faces they open.
namespace geomsrv::archviz::buildingtopology {
using namespace detail;
namespace fs = floorscheme;

namespace {
constexpr double kLook = 0.05;     // m beyond an uncovered edge, to see what lies there
constexpr double kParallel = 1e-5; // sine of the angle under which two edges are parallel
constexpr double kOnWall = 0.6;    // m: an opening drawn on the clear wall is this near its face

struct Edge {
    int cell = -1;
    Vec a, u;
    double length = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0; // bounds
    std::vector<std::pair<double, double>> covered;
};
bool InFlat (CellKind k)
{
    return k == CellKind::Room || k == CellKind::FlatRest || k == CellKind::Flat;
}
bool Circulation (CellKind k)
{
    return k == CellKind::Corridor || k == CellKind::Lobby;
}
FaceClass Classify (const Complex& complex, const Face& f)
{
    switch (f.beyond) {
        case Beyond::Outside:
        case Beyond::Courtyard:
            return FaceClass::Facade;
        case Beyond::Neighbour:
            return FaceClass::BuildingParty;
        case Beyond::Gap:
            return FaceClass::Unknown;
        case Beyond::Cell:
            break;
    }
    const auto& a = complex.cells[f.cells[0]];
    const auto& b = complex.cells[f.cells[1]];
    if (a.kind == CellKind::Core || b.kind == CellKind::Core)
        return FaceClass::CoreWall;
    if (InFlat (a.kind) && InFlat (b.kind))
        return a.unit == b.unit ? FaceClass::Internal : FaceClass::UnitParty;
    if ((InFlat (a.kind) && Circulation (b.kind)) || (Circulation (a.kind) && InFlat (b.kind)))
        return FaceClass::UnitCorridor;
    if (Circulation (a.kind) && Circulation (b.kind))
        return FaceClass::Open;
    return FaceClass::Partition;
}
int AddFace (Complex& complex, Face face)
{
    face.kind = Classify (complex, face);
    face.thickness = Thickness (face.kind);
    const int id = static_cast<int> (complex.faces.size ());
    for (int cell : face.cells)
        if (cell >= 0)
            complex.cells[cell].faces.push_back (id);
    complex.faces.push_back (std::move (face));
    return id;
}
// The face of `cells` nearest the opening a-b that runs along it within kOnWall, among those
// `accept` takes; -1 when none.
template <typename Accept>
int FaceFor (const Complex& complex, const std::vector<int>& cells, Vec a, Vec b, Accept accept)
{
    const Vec m { (a.x + b.x) / 2, (a.y + b.y) / 2 }, d = Direction ({ b.x - a.x, b.y - a.y });
    int best = -1;
    double nearest = kOnWall;
    for (int cell : cells)
        for (int id : complex.cells[cell].faces) {
            const auto& f = complex.faces[id];
            if (!accept (f))
                continue;
            const Vec u = Direction ({ f.b.x - f.a.x, f.b.y - f.a.y });
            const double length = std::hypot (f.b.x - f.a.x, f.b.y - f.a.y);
            if (std::abs (Cross (u, d)) > 1e-3)
                continue;
            const Vec w { m.x - f.a.x, m.y - f.a.y };
            const double t = Dot (w, u), off = std::abs (Cross (u, w));
            if (t < -kContact || t > length + kContact || off >= nearest)
                continue;
            nearest = off, best = id;
        }
    return best;
}
} // namespace

namespace detail {
void AddFaces (Complex& complex, int floor, const FloorInput& input)
{
    const auto& cells = complex.floors[floor].cells;
    std::vector<Edge> edges;
    for (int id : cells) {
        const auto& ring = complex.cells[id].shape;
        for (size_t i = 0; i < ring.size (); ++i) {
            const Vec a = ring[i], b = ring[(i + 1) % ring.size ()];
            Edge e;
            e.cell = id, e.a = a, e.length = std::hypot (b.x - a.x, b.y - a.y);
            if (e.length < 1e-6)
                continue;
            e.u = { (b.x - a.x) / e.length, (b.y - a.y) / e.length };
            e.x0 = (std::min) (a.x, b.x) - kCollinear, e.x1 = (std::max) (a.x, b.x) + kCollinear;
            e.y0 = (std::min) (a.y, b.y) - kCollinear, e.y1 = (std::max) (a.y, b.y) + kCollinear;
            edges.push_back (std::move (e));
        }
    }
    // Shared walls: the overlap of two collinear edges of different cells.
    for (size_t i = 0; i < edges.size (); ++i)
        for (size_t k = i + 1; k < edges.size (); ++k) {
            auto& e = edges[i];
            auto& g = edges[k];
            if (e.cell == g.cell || e.x0 > g.x1 || g.x0 > e.x1 || e.y0 > g.y1 || g.y0 > e.y1)
                continue;
            if (std::abs (Cross (e.u, g.u)) > kParallel)
                continue;
            const Vec gb { g.a.x + g.u.x * g.length, g.a.y + g.u.y * g.length };
            const Vec wa { g.a.x - e.a.x, g.a.y - e.a.y }, wb { gb.x - e.a.x, gb.y - e.a.y };
            if (std::abs (Cross (e.u, wa)) > kCollinear || std::abs (Cross (e.u, wb)) > kCollinear)
                continue;
            const double ta = Dot (wa, e.u), tb = Dot (wb, e.u);
            const double lo = (std::max) (0.0, (std::min) (ta, tb)), hi = (std::min) (e.length, (std::max) (ta, tb));
            if (hi - lo <= 1e-9)
                continue;
            e.covered.push_back ({ lo, hi });
            const Vec pa { e.a.x + e.u.x * lo, e.a.y + e.u.y * lo }, pb { e.a.x + e.u.x * hi, e.a.y + e.u.y * hi };
            const double sa = Dot ({ pa.x - g.a.x, pa.y - g.a.y }, g.u), sb = Dot ({ pb.x - g.a.x, pb.y - g.a.y }, g.u);
            g.covered.push_back ({ (std::min) (sa, sb), (std::max) (sa, sb) });
            if (hi - lo <= kContact)
                continue; // a corner touch: no wall
            Face face;
            face.floor = floor, face.a = pa, face.b = pb;
            face.cells[0] = e.cell, face.cells[1] = g.cell;
            AddFace (complex, std::move (face));
        }
    // What lies beyond the rest of each edge.
    const auto outline = ToPaths (complex.floors[floor].outline);
    const auto party = ToPaths (input.party);
    cp::PathsD holes;
    for (const auto& path : outline)
        if (cp::Area (path) < 0)
            holes.push_back (path);
    for (auto& e : edges) {
        std::sort (e.covered.begin (), e.covered.end ());
        std::vector<std::pair<double, double>> open;
        double from = 0;
        for (const auto& [lo, hi] : e.covered) {
            if (lo - from > kContact)
                open.push_back ({ from, lo });
            from = (std::max) (from, hi);
        }
        if (e.length - from > kContact)
            open.push_back ({ from, e.length });
        for (const auto& [lo, hi] : open) {
            const double mid = (lo + hi) / 2;
            const Vec n { e.u.y, -e.u.x }; // outward of a counter-clockwise ring
            const Vec p { e.a.x + e.u.x * mid + n.x * kLook, e.a.y + e.u.y * mid + n.y * kLook };
            Face face;
            face.floor = floor;
            face.a = { e.a.x + e.u.x * lo, e.a.y + e.u.y * lo };
            face.b = { e.a.x + e.u.x * hi, e.a.y + e.u.y * hi };
            face.cells[0] = e.cell;
            // A neighbour along a slanted edge that no collinear edge matched: the lower cell
            // records the face, once.
            int other = -1;
            for (int id : cells)
                if (id != e.cell && Inside ({ ToPath (complex.cells[id].shape) }, p))
                    other = id;
            if (other >= 0) {
                if (other < e.cell)
                    continue;
                face.cells[1] = other;
                complex.diagnostics.push_back (
                    { "face.oblique", "Two cells meet along edges that do not line up.", floor, p });
            }
            else if (Inside (outline, p)) {
                face.beyond = Beyond::Gap;
                complex.diagnostics.push_back (
                    { "floor.gap", "Floor inside the outline that no cell takes.", floor, p });
            }
            else if (!party.empty () && Inside (party, p))
                face.beyond = Beyond::Neighbour;
            else if (std::any_of (holes.begin (), holes.end (),
                                  [&] (const cp::PathD& hole) { return Winding (hole, p) != 0; }))
                face.beyond = Beyond::Courtyard;
            else
                face.beyond = Beyond::Outside;
            AddFace (complex, std::move (face));
        }
    }
    // The generator's openings on their faces: a window on its room's facade, the entrance
    // door on the wall between the flat and its corridor, landing or lobby.
    if (!input.scheme)
        return;
    for (int unit : complex.floors[floor].units) {
        const auto& u = complex.units[unit];
        const auto& flat = input.scheme->flats[u.flat];
        auto place = [&] (ApertureKind kind, const fs::Opening& o, int face, int cell) {
            if (face < 0) {
                complex.diagnostics.push_back ({ kind == ApertureKind::Door ? "door.unplaced" : "window.unplaced",
                                                 "An opening that lies on no wall of its flat.", floor, o.a });
                return;
            }
            const int id = static_cast<int> (complex.apertures.size ());
            complex.apertures.push_back ({ kind, face, cell, o.a, o.b });
            complex.faces[face].apertures.push_back (id);
        };
        for (const auto& w : flat.windows) {
            std::vector<int> rooms;
            for (int cell : u.cells)
                if (!u.roomsKnown || (complex.cells[cell].kind == CellKind::Room && complex.cells[cell].room == w.kind))
                    rooms.push_back (cell);
            const int face = FaceFor (complex, rooms, w.a, w.b, [] (const Face& f) {
                return f.beyond == Beyond::Outside || f.beyond == Beyond::Courtyard;
            });
            place (ApertureKind::Window, w, face, face >= 0 ? complex.faces[face].cells[0] : -1);
        }
        if (std::hypot (flat.door.b.x - flat.door.a.x, flat.door.b.y - flat.door.a.y) < 1e-6)
            continue;
        const int face = FaceFor (complex, u.cells, flat.door.a, flat.door.b, [&] (const Face& f) {
            if (f.beyond != Beyond::Cell)
                return false;
            const auto& a = complex.cells[f.cells[0]];
            const auto& b = complex.cells[f.cells[1]];
            return (a.unit == unit) != (b.unit == unit) && (Circulation (a.kind) || Circulation (b.kind) ||
                                                            a.kind == CellKind::Core || b.kind == CellKind::Core);
        });
        int cell = -1;
        if (face >= 0)
            cell = complex.cells[complex.faces[face].cells[0]].unit == unit ? complex.faces[face].cells[0]
                                                                            : complex.faces[face].cells[1];
        place (ApertureKind::Door, flat.door, face, cell);
    }
}
} // namespace detail
} // namespace geomsrv::archviz::buildingtopology
