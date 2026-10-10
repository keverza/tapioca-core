#include "ArchViz/BuildingTopologyDetail.hpp"

// Cells of each floor: a flat's rooms grown to the centre lines of the walls between them, the
// floor's circulation, stairs and unassigned pieces as the scheme drew them (already on centre
// lines: they share edges with the flats). Faces and apertures: BuildingTopologyBoundaries.cpp.
namespace geomsrv::archviz::buildingtopology {
using namespace detail;
namespace fs = floorscheme;

namespace {
// The widest gap between two room boxes, or a room box and its flat's edge, that is a wall: the
// generator leaves kInnerWall between rooms and half a party wall at the flat's edge.
constexpr double kWallGap = 0.25;
// A piece of a flat its rooms leave that is smaller than this joins the room it touches most.
constexpr double kSliver = 0.5; // m2

struct Frame {
    Vec o, u { 1, 0 }, v { 0, 1 };
    Vec Local (Vec p) const
    {
        const Vec d { p.x - o.x, p.y - o.y };
        return { Dot (d, u), Dot (d, v) };
    }
    Vec World (double s, double t) const
    {
        return { o.x + s * u.x + t * v.x, o.y + s * u.y + t * v.y };
    }
};
struct Box {
    double lo[2] = { 0, 0 }, hi[2] = { 0, 0 }; // [0]: u, [1]: v
};
// The box a rectangle is in `f`; false when it is not a rectangle along f.
bool BoxOf (const Frame& f, const Ring& ring, Box& box)
{
    if (ring.size () != 4)
        return false;
    box.lo[0] = box.lo[1] = 1e18, box.hi[0] = box.hi[1] = -1e18;
    for (const auto& p : ring) {
        const Vec q = f.Local (p);
        box.lo[0] = (std::min) (box.lo[0], q.x), box.hi[0] = (std::max) (box.hi[0], q.x);
        box.lo[1] = (std::min) (box.lo[1], q.y), box.hi[1] = (std::max) (box.hi[1], q.y);
    }
    for (const auto& p : ring) {
        const Vec q = f.Local (p);
        const bool onU = std::abs (q.x - box.lo[0]) < 1e-6 || std::abs (q.x - box.hi[0]) < 1e-6;
        const bool onV = std::abs (q.y - box.lo[1]) < 1e-6 || std::abs (q.y - box.hi[1]) < 1e-6;
        if (!onU || !onV)
            return false;
    }
    return true;
}
Ring RingOf (const Frame& f, const Box& b)
{
    return Counter ({ f.World (b.lo[0], b.lo[1]), f.World (b.hi[0], b.lo[1]), f.World (b.hi[0], b.hi[1]),
                      f.World (b.lo[0], b.hi[1]) });
}
// Distance from `p` along unit `d` to the first edge of `ring` (limit + 1 when none is nearer).
double Reach (const Ring& ring, Vec p, Vec d, double limit)
{
    double best = limit + 1;
    for (size_t i = 0; i < ring.size (); ++i) {
        const Vec a = ring[i], b = ring[(i + 1) % ring.size ()];
        const Vec e { b.x - a.x, b.y - a.y }, w { a.x - p.x, a.y - p.y };
        const double den = Cross (d, e);
        if (std::abs (den) < 1e-12)
            continue;
        const double t = Cross (w, e) / den, s = Cross (w, d) / den;
        if (t >= -1e-6 && s >= -1e-9 && s <= 1 + 1e-9)
            best = (std::min) (best, (std::max) (0.0, t));
    }
    return best;
}
// Boundary shared by two polygons, from the overlap of a thin band around `a`.
double Touch (const Ring& a, const Ring& b)
{
    constexpr double band = 0.02;
    const auto grown =
        cp::InflatePaths ({ ToPath (a) }, band, cp::JoinType::Miter, cp::EndType::Polygon, 2.0, kPrecision);
    return std::abs (Area (cp::Intersect (grown, { ToPath (b) }, cp::FillRule::NonZero, kPrecision))) / band;
}

struct Tile {
    Ring shape;
    int room = -1; // index in Flat::roomList; -1: a piece the rooms leave
};
// `flat`'s rooms grown to the centre lines of the walls between them and to the flat's edge; what
// they leave joins a room (a sliver) or is a piece of its own. False when they do not tile it.
bool TileRooms (const fs::Flat& flat, std::vector<Tile>& out)
{
    out.clear ();
    const auto& rooms = flat.roomList;
    if (flat.manual || rooms.empty () || rooms.front ().shape.size () != 4 || flat.shape.size () < 3)
        return false;
    Frame f;
    f.o = rooms.front ().shape[0];
    f.u = Direction ({ rooms.front ().shape[1].x - f.o.x, rooms.front ().shape[1].y - f.o.y });
    f.v = { -f.u.y, f.u.x };
    std::vector<Box> boxes (rooms.size ());
    for (size_t i = 0; i < rooms.size (); ++i)
        if (!BoxOf (f, rooms[i].shape, boxes[i]))
            return false;
    const Ring shape = Counter (flat.shape);
    std::vector<Box> grown = boxes;
    for (size_t i = 0; i < boxes.size (); ++i)
        for (int axis = 0; axis < 2; ++axis)
            for (int high = 0; high < 2; ++high) {
                const Box& b = boxes[i];
                const double side = high ? b.hi[axis] : b.lo[axis], sign = high ? 1.0 : -1.0;
                const int across = 1 - axis;
                // The nearest room beyond this side: the wall between them is shared, half each.
                double reach = 1e18;
                for (size_t k = 0; k < boxes.size (); ++k) {
                    if (k == i)
                        continue;
                    const Box& c = boxes[k];
                    const double facing = high ? c.lo[axis] : c.hi[axis];
                    const double gap = sign * (facing - side);
                    const double overlap =
                        (std::min) (b.hi[across], c.hi[across]) - (std::max) (b.lo[across], c.lo[across]);
                    if (gap >= -1e-6 && gap <= kWallGap && overlap > kContact)
                        reach = (std::min) (reach, (std::max) (0.0, gap) / 2);
                }
                if (reach < 1e17) {
                    (high ? grown[i].hi : grown[i].lo)[axis] = side + sign * reach;
                    continue;
                }
                // Else the flat's edge, when the whole side is within a wall of it.
                double far = 0;
                for (int s = 1; s <= 5; ++s) {
                    const double t = b.lo[across] + (b.hi[across] - b.lo[across]) * s / 6.0;
                    const Vec p = axis == 0 ? f.World (side, t) : f.World (t, side);
                    const Vec d = axis == 0 ? Vec { f.u.x * sign, f.u.y * sign } : Vec { f.v.x * sign, f.v.y * sign };
                    far = (std::max) (far, Reach (shape, p, d, kWallGap));
                }
                if (far <= kWallGap)
                    (high ? grown[i].hi : grown[i].lo)[axis] = side + sign * far;
            }
    const cp::PathsD whole { ToPath (shape) };
    cp::PathsD kept;
    for (size_t i = 0; i < grown.size (); ++i) {
        auto parts = cp::Intersect ({ ToPath (RingOf (f, grown[i])) }, whole, cp::FillRule::NonZero, kPrecision);
        if (parts.empty ())
            return false;
        // A room split by a notch of the flat keeps its largest part; the rest is left over.
        const auto largest = std::max_element (parts.begin (), parts.end (), [] (const auto& a, const auto& b) {
            return std::abs (cp::Area (a)) < std::abs (cp::Area (b));
        });
        out.push_back ({ Counter (FromPath (*largest)), static_cast<int> (i) });
        kept.push_back (*largest);
    }
    for (const auto& piece : cp::Difference (whole, kept, cp::FillRule::NonZero, kPrecision)) {
        const double area = std::abs (cp::Area (piece));
        if (area < 1e-4)
            continue;
        Ring ring = Counter (FromPath (piece));
        if (area < kSliver) {
            size_t best = out.size ();
            double most = kContact;
            for (size_t k = 0; k < out.size (); ++k)
                if (const double t = Touch (ring, out[k].shape); t > most)
                    most = t, best = k;
            if (best < out.size ()) {
                const auto joined =
                    cp::Union ({ ToPath (out[best].shape), ToPath (ring) }, cp::FillRule::NonZero, kPrecision);
                if (joined.size () == 1) {
                    out[best].shape = Counter (FromPath (cp::SimplifyPath (joined.front (), 1e-4)));
                    continue;
                }
            }
        }
        out.push_back ({ std::move (ring), -1 });
    }
    // The tiles must cover the flat once: no overlap, nothing lost.
    cp::PathsD all;
    double sum = 0;
    for (const auto& t : out)
        all.push_back (ToPath (t.shape)), sum += std::abs (fs::Area (t.shape));
    const double flatArea = std::abs (fs::Area (shape));
    const double tolerance = (std::max) (0.05, 0.002 * flatArea);
    const double joined = std::abs (Area (cp::Union (all, cp::FillRule::NonZero, kPrecision)));
    return std::abs (sum - flatArea) <= tolerance && std::abs (sum - joined) <= tolerance;
}
} // namespace

namespace detail {
void AddCells (Complex& complex, int floor, const FloorInput& input, const std::vector<fs::Pins::Core>& stack)
{
    if (!input.scheme)
        return;
    const auto& s = *input.scheme;
    auto add = [&] (CellKind kind, const Ring& shape, int unit, fs::RoomKind room = fs::RoomKind::Living) {
        Cell cell;
        cell.kind = kind, cell.room = room, cell.floor = floor, cell.unit = unit;
        cell.shape = Counter (shape);
        cell.area = std::abs (fs::Area (cell.shape));
        if (cell.shape.size () < 3 || cell.area < 1e-4)
            return -1;
        const int id = static_cast<int> (complex.cells.size ());
        complex.cells.push_back (std::move (cell));
        complex.floors[floor].cells.push_back (id);
        if (unit >= 0)
            complex.units[unit].cells.push_back (id);
        return id;
    };
    std::vector<Tile> tiles;
    for (size_t i = 0; i < s.flats.size (); ++i) {
        const auto& flat = s.flats[i];
        Unit unit;
        unit.floor = floor, unit.flat = static_cast<int> (i), unit.type = flat.type, unit.rooms = flat.rooms;
        const int id = static_cast<int> (complex.units.size ());
        complex.units.push_back (unit);
        complex.floors[floor].units.push_back (id);
        if (TileRooms (flat, tiles)) {
            complex.units[id].roomsKnown = true;
            for (const auto& t : tiles)
                add (t.room >= 0 ? CellKind::Room : CellKind::FlatRest, t.shape, id,
                     t.room >= 0 ? flat.roomList[t.room].kind : fs::RoomKind::Living);
            continue;
        }
        if (!flat.manual && !flat.roomList.empty ())
            complex.diagnostics.push_back ({ "rooms.untiled", "A flat's rooms do not tile it: kept as one cell.", floor,
                                             flat.shape.empty () ? Vec {} : flat.shape.front () });
        add (CellKind::Flat, flat.shape, id);
    }
    for (const auto& c : s.corridors)
        add (CellKind::Corridor, c.shape, -1);
    for (const auto& l : s.lobbies)
        add (CellKind::Lobby, l, -1);
    for (const auto& core : s.cores) {
        const int id = add (CellKind::Core, core.shape, -1);
        if (id < 0)
            continue;
        // The stack stair it is: held stairs stand exactly where pinned (Options::holdCores).
        double near = 0.5;
        for (size_t k = 0; k < stack.size (); ++k)
            if (const double d = std::hypot (stack[k].centre.x - core.centre.x, stack[k].centre.y - core.centre.y);
                d < near)
                near = d, complex.cells[id].stack = static_cast<int> (k);
        if (!stack.empty () && complex.cells[id].stack < 0)
            complex.diagnostics.push_back (
                { "core.not_in_stack", "A stair the building's stack does not have.", floor, core.centre });
    }
    for (const auto& u : s.unassigned)
        add (CellKind::Unassigned, u.shape, -1);
}
} // namespace detail

Complex Build (const std::vector<FloorInput>& floors, const std::vector<fs::Pins::Core>& stack)
{
    Complex complex;
    for (const auto& input : floors) {
        Floor floor;
        floor.story = input.story, floor.z = input.z, floor.height = input.height;
        if (input.scheme)
            floor.outline = input.scheme->outline;
        const int id = static_cast<int> (complex.floors.size ());
        complex.floors.push_back (std::move (floor));
        AddCells (complex, id, input, stack);
        AddFaces (complex, id, input);
    }
    return complex;
}

double Thickness (FaceClass kind)
{
    switch (kind) {
        case FaceClass::Internal:
            return fs::kInnerWall;
        case FaceClass::UnitParty:
        case FaceClass::UnitCorridor:
        case FaceClass::CoreWall:
        case FaceClass::Partition:
            return floorprogramme::kWall;
        case FaceClass::Facade:
            return floorprogramme::kFacade;
        case FaceClass::BuildingParty:
            return floorprogramme::kFacade; // one wall for both buildings, half each
        case FaceClass::Open:
        case FaceClass::Unknown:
            return 0;
    }
    return 0;
}
double Share (const Complex& complex, int face, int cell)
{
    if (face < 0 || face >= static_cast<int> (complex.faces.size ()))
        return 0;
    const auto& f = complex.faces[face];
    if (cell < 0 || (f.cells[0] != cell && f.cells[1] != cell))
        return 0;
    return f.kind == FaceClass::Facade ? f.thickness : f.thickness / 2;
}
std::vector<Ring> Union (const Complex& complex, const std::vector<int>& cells)
{
    cp::PathsD paths;
    for (int id : cells)
        if (id >= 0 && id < static_cast<int> (complex.cells.size ()))
            paths.push_back (ToPath (complex.cells[id].shape));
    std::vector<Ring> out;
    for (const auto& path : cp::Union (paths, cp::FillRule::NonZero, kPrecision))
        out.push_back (FromPath (cp::SimplifyPath (path, 1e-4)));
    return out;
}
} // namespace geomsrv::archviz::buildingtopology
