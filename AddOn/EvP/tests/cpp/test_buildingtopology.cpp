// A building's planned floors as one cell complex (BuildingTopology.hpp): cells that tile each
// floor without overlap, flats as sets of room cells, one face per stretch of wall.
#include "ArchViz/BuildingTopology.hpp"
#include "NodeGraph/Json.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace bt = geomsrv::archviz::buildingtopology;
namespace fs = geomsrv::archviz::floorscheme;
namespace fp = geomsrv::archviz::floorprogramme;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
fs::Ring Reversed (fs::Ring ring)
{
    std::reverse (ring.begin (), ring.end ());
    return ring;
}
double Length (const bt::Face& f)
{
    return std::hypot (f.b.x - f.a.x, f.b.y - f.a.y);
}
double Area (const std::vector<fs::Ring>& rings)
{
    double a = 0;
    for (const auto& r : rings)
        a += fs::Area (r);
    return a;
}
std::string Codes (const bt::Complex& c)
{
    std::string out;
    for (const auto& d : c.diagnostics)
        out += d.code + " ";
    return out;
}
// A scheme of only flats, for exact wall layouts.
fs::Scheme Flats (const std::vector<fs::Ring>& outline, const std::vector<fs::Ring>& flats)
{
    fs::Scheme s;
    s.outline = outline;
    for (const auto& r : flats) {
        fs::Flat f;
        f.shape = r;
        f.manual = true; // rooms left to the user: one cell each
        s.flats.push_back (f);
    }
    return s;
}
} // namespace

TEST (BuildingTopology, CellsTileEveryStandardFloorAndFlatsAreTheirRooms)
{
    const std::vector<std::pair<const char*, std::vector<fs::Ring>>> cases {
        { "I 60 x 16", { Rect (0, 0, 60, 16) } },
        { "I 48 x 12, sections", { Rect (0, 0, 48, 12) } },
        { "L", { Rect (0, 0, 48, 16), Rect (0, 16, 16, 48) } },
        { "O", { Rect (0, 0, 50, 50), Reversed (Rect (14, 14, 36, 36)) } },
        { "T", { Rect (0, 0, 60, 16), Rect (22, 16, 38, 40) } },
        { "slanted end", { { { 0, 0 }, { 50, 0 }, { 56, 16 }, { 0, 16 } } } },
    };
    for (const auto& [name, outline] : cases) {
        const auto s = fs::Generate (outline, fp::Default ());
        ASSERT_TRUE (s.ok) << name;
        const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
        EXPECT_TRUE (c.diagnostics.empty ()) << name << ": " << Codes (c);
        for (const auto& d : bt::Check (c))
            ADD_FAILURE () << name << ": " << d.code;
        double cells = 0;
        for (const auto& cell : c.cells)
            cells += cell.area;
        EXPECT_NEAR (cells, Area (s.outline), 0.05) << name << ": cells tile the floor once";
        EXPECT_NEAR (Area (bt::Union (c, c.floors[0].cells)), Area (s.outline), 0.05) << name << ": no overlap";
        ASSERT_EQ (c.units.size (), s.flats.size ()) << name;
        for (const auto& u : c.units) {
            const auto& flat = s.flats[u.flat];
            EXPECT_EQ (u.roomsKnown, !flat.manual) << name;
            EXPECT_NEAR (Area (bt::Union (c, u.cells)), std::abs (fs::Area (flat.shape)), 0.05)
                << name << ": a flat is the union of its cells";
        }
        // Every wall classified; the facade is the whole outline.
        double facade = 0, perimeter = 0;
        for (const auto& f : c.faces) {
            EXPECT_NE (f.kind, bt::FaceClass::Unknown) << name;
            if (f.kind == bt::FaceClass::Facade)
                facade += Length (f);
        }
        for (const auto& r : s.outline)
            for (size_t i = 0; i < r.size (); ++i)
                perimeter += std::hypot (r[(i + 1) % r.size ()].x - r[i].x, r[(i + 1) % r.size ()].y - r[i].y);
        EXPECT_NEAR (facade, perimeter, 1e-3 * perimeter) << name << ": slanted edges lose a little to rounding";
    }
}

TEST (BuildingTopology, ALongWallMeetingTwoShortOnesIsTwoFacesWithTwoCellsEach)
{
    const auto s = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 10, 2), Rect (0, 2, 4, 4), Rect (4, 2, 10, 4) });
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    EXPECT_TRUE (c.diagnostics.empty ()) << Codes (c);
    std::vector<double> party;
    for (const auto& f : c.faces)
        if (f.kind == bt::FaceClass::UnitParty) {
            EXPECT_EQ (f.beyond, bt::Beyond::Cell);
            EXPECT_NE (f.cells[0], f.cells[1]);
            EXPECT_GE (f.cells[1], 0);
            party.push_back (Length (f));
        }
    std::sort (party.begin (), party.end ());
    ASSERT_EQ (party.size (), 3u) << "4 m and 6 m under the long flat, 2 m between the short ones";
    EXPECT_NEAR (party[0], 2, 1e-6);
    EXPECT_NEAR (party[1], 4, 1e-6);
    EXPECT_NEAR (party[2], 6, 1e-6);
    // Reversed rings and another order give the same walls.
    const auto r = Flats ({ Rect (0, 0, 10, 4) },
                          { Reversed (Rect (4, 2, 10, 4)), Rect (0, 2, 4, 4), Reversed (Rect (0, 0, 10, 2)) });
    const auto d = bt::Build ({ { 0, 0, 3, &r, {} } });
    EXPECT_EQ (d.faces.size (), c.faces.size ());
}

TEST (BuildingTopology, ACornerTouchIsNoWall)
{
    // Two flats meeting at one point, the rest of the floor two more.
    const auto s =
        Flats ({ Rect (0, 0, 4, 4) }, { Rect (0, 0, 2, 2), Rect (2, 2, 4, 4), Rect (2, 0, 4, 2), Rect (0, 2, 2, 4) });
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    for (const auto& f : c.faces)
        if (f.beyond == bt::Beyond::Cell) {
            const std::pair<int, int> pair { (std::min) (f.cells[0], f.cells[1]), (std::max) (f.cells[0], f.cells[1]) };
            EXPECT_NE (pair, std::make_pair (0, 1)) << "diagonal flats share no wall";
            EXPECT_NE (pair, std::make_pair (2, 3));
        }
}

TEST (BuildingTopology, CourtyardNeighbourAndGapAreTold)
{
    // A floor with a courtyard, against a neighbouring building on the west, one piece missing.
    const auto s = Flats ({ Rect (0, 0, 12, 12), Reversed (Rect (4, 4, 8, 8)) },
                          { Rect (0, 0, 12, 4), Rect (0, 4, 4, 12), Rect (8, 4, 12, 12), Rect (4, 8, 8, 12) });
    auto missing = s;
    missing.flats.pop_back ();
    const std::vector<fs::Ring> party { Rect (-10, 0, 0, 12) };
    const auto c = bt::Build ({ { 0, 0, 3, &s, party } });
    std::map<bt::Beyond, double> beyond;
    for (const auto& f : c.faces)
        beyond[f.beyond] += Length (f);
    EXPECT_NEAR (beyond[bt::Beyond::Courtyard], 16, 1e-6);
    EXPECT_NEAR (beyond[bt::Beyond::Neighbour], 12, 1e-6);
    EXPECT_NEAR (beyond[bt::Beyond::Outside], 36, 1e-6);
    EXPECT_EQ (beyond.count (bt::Beyond::Gap), 0u);
    for (const auto& f : c.faces)
        if (f.beyond == bt::Beyond::Neighbour) {
            EXPECT_EQ (f.kind, bt::FaceClass::BuildingParty);
            EXPECT_NEAR (bt::Share (c, static_cast<int> (&f - c.faces.data ()), f.cells[0]), fp::kFacade / 2, 1e-9)
                << "half a party wall is each building's";
        }
    const auto g = bt::Build ({ { 0, 0, 3, &missing, party } });
    EXPECT_NE (Codes (g).find ("floor.gap"), std::string::npos) << "a gap is reported, never closed";
}

TEST (BuildingTopology, WallsTakeTheirThicknessAndShare)
{
    const auto s = fs::Generate ({ Rect (0, 0, 36, 16) }, fp::Default ());
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    std::map<bt::FaceClass, int> seen;
    for (size_t i = 0; i < c.faces.size (); ++i) {
        const auto& f = c.faces[i];
        ++seen[f.kind];
        const double share = bt::Share (c, static_cast<int> (i), f.cells[0]);
        switch (f.kind) {
            case bt::FaceClass::Facade:
                EXPECT_NEAR (share, 0.5, 1e-9) << "the facade wall stands inside the outline";
                break;
            case bt::FaceClass::UnitParty:
            case bt::FaceClass::UnitCorridor:
            case bt::FaceClass::CoreWall:
                EXPECT_NEAR (share, 0.1, 1e-9) << "a 0.2 m partition, half each side";
                break;
            case bt::FaceClass::Internal:
                EXPECT_NEAR (share, 0.05, 1e-9);
                break;
            default:
                break;
        }
    }
    EXPECT_GT (seen[bt::FaceClass::UnitParty], 0);
    EXPECT_GT (seen[bt::FaceClass::UnitCorridor], 0);
    EXPECT_GT (seen[bt::FaceClass::CoreWall], 0);
    EXPECT_GT (seen[bt::FaceClass::Internal], 0);
    EXPECT_EQ (bt::Share (c, 0, -1), 0.0);
}

TEST (BuildingTopology, WindowsAreOnTheirRoomsFacadeAndDoorsOnCirculation)
{
    const auto s = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default ());
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    size_t windows = 0, doors = 0;
    for (const auto& f : s.flats)
        windows += f.windows.size (), doors += 1;
    size_t placedWindows = 0, placedDoors = 0;
    for (const auto& a : c.apertures) {
        const auto& f = c.faces[a.face];
        ASSERT_GE (a.cell, 0);
        if (a.kind == bt::ApertureKind::Window) {
            ++placedWindows;
            EXPECT_EQ (f.kind, bt::FaceClass::Facade);
            EXPECT_EQ (c.cells[a.cell].kind, bt::CellKind::Room);
        }
        else {
            ++placedDoors;
            EXPECT_TRUE (f.kind == bt::FaceClass::UnitCorridor || f.kind == bt::FaceClass::CoreWall);
            EXPECT_GE (c.cells[a.cell].unit, 0) << "the door enters its flat";
        }
    }
    EXPECT_EQ (placedWindows, windows);
    EXPECT_EQ (placedDoors, doors);
}

TEST (BuildingTopology, EveryFloorsStairIsTheStacksStair)
{
    const auto free = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default ());
    fs::Pins pins;
    for (const auto& core : free.cores)
        pins.cores.push_back ({ core.centre, core.width, core.depth });
    fs::Options held;
    held.holdCores = true;
    const auto ground = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default (), pins, held);
    const auto top = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default (), pins, held);
    const auto c = bt::Build ({ { 0, 0, 3, &ground, {} }, { 1, 3, 3, &top, {} } }, pins.cores);
    ASSERT_EQ (c.floors.size (), 2u);
    std::map<int, std::vector<int>> stacks; // stack stair -> floors
    for (const auto& cell : c.cells)
        if (cell.kind == bt::CellKind::Core) {
            ASSERT_GE (cell.stack, 0);
            stacks[cell.stack].push_back (cell.floor);
        }
    ASSERT_EQ (stacks.size (), pins.cores.size ());
    for (const auto& [stair, floors] : stacks)
        EXPECT_EQ (floors, (std::vector<int> { 0, 1 })) << "stair " << stair << " on every floor";
}

// User, 2026-10-10: the facade wall 0.5 m inside the massing, 0.2 m partitions, a 0.3 m slab below.
TEST (BuildingTopology, ClearGeometryStandsInsideTheWalls)
{
    const auto s = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 10, 2), Rect (0, 2, 4, 4), Rect (4, 2, 10, 4) });
    bt::FloorInput input { 2, 6.0, 3.0, &s, {} };
    const auto c = bt::Build ({ input });
    // The long flat: facade on three sides (0.5 m in), party wall on the fourth (0.1 m).
    const auto flat = bt::Clear (c, c.units[0].cells);
    ASSERT_EQ (flat.size (), 1u);
    EXPECT_NEAR (Area (flat), (10 - 1.0) * (2 - 0.5 - 0.1), 1e-4);
    // Two flats together lose the wall between them, not its thickness on the outside.
    const auto both = bt::Clear (c, { c.units[1].cells[0], c.units[2].cells[0] });
    EXPECT_NEAR (Area (both), (10 - 1.0) * (2 - 0.5 - 0.1), 1e-4);
    // The slab: the floor inside its facade, 0.3 m thick below the floor's level.
    EXPECT_NEAR (Area (bt::Plate (c, 0)), 9 * 3, 1e-4);
    EXPECT_NEAR (bt::SlabSpan (c.floors[0]).z0, 5.7, 1e-9);
    EXPECT_NEAR (bt::SlabSpan (c.floors[0]).z1, 6.0, 1e-9);
    EXPECT_NEAR (bt::ClearSpan (c.floors[0]).z0, 6.0, 1e-9);
    EXPECT_NEAR (bt::ClearSpan (c.floors[0]).z1, 8.7, 1e-9);
}

TEST (BuildingTopology, AReEntrantCornerKeepsTheWallCorner)
{
    // An L flat on its own: its clear shape is the L moved 0.5 m in all round.
    const fs::Ring l { { 0, 0 }, { 10, 0 }, { 10, 4 }, { 4, 4 }, { 4, 10 }, { 0, 10 } };
    const auto s = Flats ({ l }, { l });
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    const auto clear = bt::Clear (c, c.units[0].cells);
    ASSERT_EQ (clear.size (), 1u);
    // (10 - 1) x (4 - 1) plus (4 - 1) x (10 - 4): the arm above the bar's clear part.
    EXPECT_NEAR (Area (clear), 9 * 3 + 3 * 6, 1e-4);
}

// Sharing a wall is not a way through it; doors, open circulation and stairs are.
TEST (BuildingTopology, GraphsTellWallsFromWaysThrough)
{
    const auto s = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default ());
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    EXPECT_TRUE (bt::Check (c).empty ());
    const auto adjacency = bt::Adjacency (c), access = bt::Access (c), exterior = bt::Exterior (c);
    size_t doors = 0, partyWalls = 0;
    for (const auto& link : access.links) {
        doors += link.aperture >= 0;
        const auto& a = c.cells[link.from];
        const auto& b = c.cells[link.to];
        EXPECT_FALSE (a.unit >= 0 && b.unit >= 0 && a.unit != b.unit) << "no way from one flat into another";
    }
    for (const auto& link : adjacency.links)
        partyWalls += c.cells[link.from].unit >= 0 && c.cells[link.to].unit >= 0 &&
                      c.cells[link.from].unit != c.cells[link.to].unit;
    EXPECT_EQ (doors, s.flats.size ()) << "one entrance door per flat";
    EXPECT_GT (partyWalls, 0u) << "flats share walls";
    // Windows give exterior contact, never access.
    size_t windows = 0;
    for (const auto& link : exterior.links) {
        EXPECT_EQ (link.to, -1);
        windows += link.aperture >= 0;
    }
    EXPECT_GT (windows, 0u);
    for (size_t u = 0; u < c.units.size (); ++u) {
        const auto f = bt::Figures (c, static_cast<int> (u));
        EXPECT_TRUE (f.entrance && f.reachesStair) << u;
        EXPECT_GT (f.frontage, 0) << u;
        EXPECT_GT (f.clearArea, 0) << u;
        EXPECT_LT (f.clearArea, std::abs (fs::Area (s.flats[c.units[u].flat].shape))) << "inside its walls";
    }
}

TEST (BuildingTopology, AStairMissingOnAFloorBreaksTheStack)
{
    const auto free = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default ());
    fs::Pins pins;
    for (const auto& core : free.cores)
        pins.cores.push_back ({ core.centre, core.width, core.depth });
    fs::Options held;
    held.holdCores = true;
    const auto ground = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default (), pins, held);
    auto top = ground;
    top.cores.pop_back (); // a stair the top floor lost
    const auto whole = bt::Build ({ { 0, 0, 3, &ground, {} }, { 1, 3, 3, &ground, {} } }, pins.cores);
    size_t vertical = 0;
    for (const auto& link : bt::Access (whole).links)
        if (whole.cells[link.from].floor != whole.cells[link.to].floor) {
            ++vertical;
            ASSERT_GE (link.face, 0) << "through the stair's shaft face";
            EXPECT_EQ (whole.faces[link.face].kind, bt::FaceClass::Shaft);
        }
    EXPECT_EQ (vertical, pins.cores.size ()) << "each stair to itself one floor up";
    EXPECT_TRUE (bt::Check (whole).empty ());
    const auto broken = bt::Build ({ { 0, 0, 3, &ground, {} }, { 1, 3, 3, &top, {} } }, pins.cores);
    const auto found = bt::Check (broken);
    EXPECT_TRUE (std::any_of (found.begin (), found.end (),
                              [] (const bt::Diagnostic& d) { return d.code == "core.broken_stack" && d.floor == 1; }));
}

TEST (BuildingTopology, AnInnerBedroomHasNoDaylightAndAFlatWithoutADoorNoEntrance)
{
    // A flat walled in by four others: its one bedroom touches no facade, and it has no door.
    auto s = Flats ({ Rect (0, -4, 12, 8) }, { Rect (0, -4, 12, 0), Rect (0, 4, 12, 8), Rect (0, 0, 4, 4),
                                               Rect (8, 0, 12, 4), Rect (4, 0, 8, 4) });
    auto& inner = s.flats.back ();
    inner.manual = false;
    inner.roomList.push_back ({ Rect (4.1, 0.1, 7.9, 3.9), fs::RoomKind::Bedroom, 3.8, 3.8 });
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    EXPECT_TRUE (c.units.back ().roomsKnown);
    const auto found = bt::Check (c);
    auto count = [&] (const char* code) {
        return std::count_if (found.begin (), found.end (), [&] (const bt::Diagnostic& d) { return d.code == code; });
    };
    EXPECT_EQ (count ("room.no_daylight"), 1);
    EXPECT_EQ (count ("unit.no_entrance"), 5) << "none of these flats has a door";
}

// A part of a floor on its own: what surrounds it classifies its walls and is never counted.
TEST (BuildingTopology, ContextClassifiesThePartsWallsAndIsNeverCounted)
{
    // One flat; a corridor below it, a stair to the east, a flat to the west, open air above.
    const auto s = Flats ({ Rect (0, 0, 6, 8) }, { Rect (0, 0, 6, 8) });
    const bt::FloorInput input { 0,
                                 0,
                                 3,
                                 &s,
                                 {},
                                 { { bt::CellKind::Corridor, Rect (-6, -2, 12, 0) },
                                   { bt::CellKind::Core, Reversed (Rect (6, 0, 10, 8)) },
                                   { bt::CellKind::Flat, Rect (-6, 0, 0, 8) } } };
    const auto c = bt::Build ({ input });
    EXPECT_TRUE (c.diagnostics.empty ()) << Codes (c);
    ASSERT_EQ (c.floors[0].cells.size (), 1u);
    ASSERT_EQ (c.floors[0].context.size (), 3u);
    for (int id : c.floors[0].context)
        EXPECT_TRUE (c.cells[id].context && c.cells[id].unit < 0);
    std::map<bt::FaceClass, double> walls;
    for (const auto& f : c.faces)
        walls[f.kind] += Length (f);
    const auto walled =
        std::count_if (c.faces.begin (), c.faces.end (), [] (const bt::Face& f) { return !f.horizontal; });
    EXPECT_EQ (walled, 4) << "only the flat's walls: none between or beyond the context";
    EXPECT_EQ (c.faces.size (), 6u) << "and its ground and roof: the context has no slab";
    EXPECT_NEAR (walls[bt::FaceClass::Facade], 6, 1e-6);
    EXPECT_NEAR (walls[bt::FaceClass::UnitCorridor], 6, 1e-6);
    EXPECT_NEAR (walls[bt::FaceClass::CoreWall], 8, 1e-6);
    EXPECT_NEAR (walls[bt::FaceClass::UnitParty], 8, 1e-6);
    // The floor is the flat: its slab stops inside the walls it shares with the context.
    EXPECT_NEAR (Area (bt::Union (c, c.floors[0].cells)), 48, 1e-6);
    EXPECT_NEAR (Area (bt::Plate (c, 0)), (6 - 0.2) * (8 - 0.6), 1e-4);
    // What lies beyond the flat, as its rooms would be laid against it.
    std::map<fs::Side::Kind, double> sides;
    for (const auto& side : bt::Sides (c, 0))
        sides[side.kind] += std::hypot (side.b.x - side.a.x, side.b.y - side.a.y);
    EXPECT_NEAR (sides[fs::Side::Facade], 6, 1e-6);
    EXPECT_NEAR (sides[fs::Side::Corridor], 6, 1e-6);
    EXPECT_NEAR (sides[fs::Side::Stair], 8, 1e-6);
    EXPECT_NEAR (sides[fs::Side::Blind], 8, 1e-6);
    // Around the flat is everything else on its floor.
    const auto around = bt::Around (c, 0);
    EXPECT_EQ (around.size (), 3u);
}

namespace {
// Area of the horizontal faces of `kind`, at elevation `z`.
double Slabs (const bt::Complex& c, bt::FaceClass kind, double z)
{
    double sum = 0;
    for (size_t i = 0; i < c.faces.size (); ++i)
        if (c.faces[i].horizontal && c.faces[i].kind == kind && std::abs (c.faces[i].z0 - z) < 1e-9)
            sum += bt::FaceArea (c, static_cast<int> (i));
    return sum;
}
} // namespace

// Cells are solids: a slab between two stacked cells is one face both of them have.
TEST (BuildingTopology, CellsAreSolidsAndASlabIsAFaceTheCellsAboveAndBelowShare)
{
    const auto lower = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 10, 4) });
    const auto upper = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 4, 4), Rect (4, 0, 10, 4) });
    const auto c = bt::Build ({ { 0, 0, 3, &lower, {} }, { 1, 3, 3, &upper, {} } });
    EXPECT_TRUE (c.diagnostics.empty ()) << Codes (c);
    ASSERT_EQ (c.cells.size (), 3u);
    EXPECT_NEAR (c.cells[0].z0, 0, 1e-9);
    EXPECT_NEAR (c.cells[0].z1, 3, 1e-9);
    EXPECT_NEAR (bt::Volume (c, 0), 10 * 4 * 3, 1e-6);
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Ground, 0), 40, 1e-6);
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Slab, 3), 40, 1e-6) << "two patches, one under each flat above";
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Roof, 6), 40, 1e-6);
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Roof, 3), 0, 1e-9) << "the lower floor is covered";
    for (int above : { 1, 2 }) {
        const auto shared = bt::Shared (c, 0, above);
        ASSERT_EQ (shared.size (), 1u);
        const auto& slab = c.faces[shared.front ()];
        EXPECT_TRUE (slab.horizontal);
        EXPECT_EQ (slab.cells[0], 0) << "below";
        EXPECT_EQ (slab.cells[1], above) << "above";
        EXPECT_NEAR (slab.thickness, fp::kSlab, 1e-9);
        EXPECT_NEAR (bt::Share (c, shared.front (), 0), fp::kSlab, 1e-9) << "in the top of the cell below";
        EXPECT_NEAR (bt::Share (c, shared.front (), above), 0, 1e-9);
    }
    auto neighbours = bt::Neighbours (c, 0);
    std::sort (neighbours.begin (), neighbours.end ());
    EXPECT_EQ (neighbours, (std::vector<int> { 1, 2 }));
    // The building as one set: its envelope is facade, ground and roof; inside, the slab and the
    // party wall between the upper flats.
    const std::vector<int> all { 0, 1, 2 };
    double envelope = 0;
    for (int face : bt::Boundary (c, all))
        envelope += bt::FaceArea (c, face);
    EXPECT_NEAR (envelope, 2 * (10 + 4) * 6 + 40 + 40, 1e-6);
    double inside = 0;
    for (int face : bt::Internal (c, all))
        inside += bt::FaceArea (c, face);
    EXPECT_NEAR (inside, 40 + 4 * 3, 1e-6);
    // Walls have the storey's height; plans stay inside the walls, whatever lies above.
    for (const auto& f : c.faces)
        if (!f.horizontal)
            EXPECT_NEAR (f.z1 - f.z0, 3, 1e-9);
    EXPECT_NEAR (Area (bt::Plate (c, 0)), 9 * 3, 1e-4);
}

TEST (BuildingTopology, ASetbackLeavesATerraceRoofAndAnOverhangAnUnderside)
{
    const auto lower = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 10, 4) });
    const auto upper = Flats ({ Rect (2, 0, 14, 4) }, { Rect (2, 0, 14, 4) });
    const auto c = bt::Build ({ { 0, 0, 3, &lower, {} }, { 1, 3, 3, &upper, {} } });
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Slab, 3), 8 * 4, 1e-6);
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Roof, 3), 2 * 4, 1e-6) << "a terrace";
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Underside, 3), 4 * 4, 1e-6) << "an overhang";
    EXPECT_NEAR (Slabs (c, bt::FaceClass::Roof, 6), 12 * 4, 1e-6);
    for (const auto& f : c.faces)
        if (f.horizontal && f.kind == bt::FaceClass::Underside)
            EXPECT_EQ (f.beyond, bt::Beyond::Outside);
    // A floor that does not stand on the one below shares no slab with it.
    const auto apart = bt::Build ({ { 0, 0, 3, &lower, {} }, { 1, 4, 3, &upper, {} } });
    EXPECT_NEAR (Slabs (apart, bt::FaceClass::Slab, 3), 0, 1e-9);
    EXPECT_NEAR (Slabs (apart, bt::FaceClass::Roof, 3), 40, 1e-6);
    EXPECT_NEAR (Slabs (apart, bt::FaceClass::Underside, 4), 48, 1e-6);
    // Floor above that no cell takes is a gap, never a roof.
    auto missing = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 4, 4), Rect (4, 0, 10, 4) });
    missing.flats.pop_back ();
    const auto gap = bt::Build ({ { 0, 0, 3, &lower, {} }, { 1, 3, 3, &missing, {} } });
    EXPECT_NEAR (Slabs (gap, bt::FaceClass::Unknown, 3), 24, 1e-6);
    EXPECT_NEAR (Slabs (gap, bt::FaceClass::Roof, 3), 0, 1e-9);
}

// The stair of the stack on consecutive floors shares an open shaft face, which the access
// graph's stair link crosses; slabs give adjacency, never access, and roofs are no facade.
TEST (BuildingTopology, AStackStairSharesItsShaftWithItselfOneFloorUp)
{
    const auto free = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default ());
    fs::Pins pins;
    for (const auto& core : free.cores)
        pins.cores.push_back ({ core.centre, core.width, core.depth });
    fs::Options held;
    held.holdCores = true;
    const auto floor = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default (), pins, held);
    const auto c = bt::Build ({ { 0, 0, 3, &floor, {} }, { 1, 3, 3, &floor, {} } }, pins.cores);
    size_t shafts = 0;
    for (const auto& f : c.faces)
        if (f.kind == bt::FaceClass::Shaft) {
            ++shafts;
            EXPECT_EQ (c.cells[f.cells[0]].stack, c.cells[f.cells[1]].stack);
            EXPECT_EQ (f.thickness, 0) << "an opening, no slab";
        }
    EXPECT_EQ (shafts, pins.cores.size ());
    size_t slabLinks = 0;
    for (const auto& link : bt::Adjacency (c).links)
        slabLinks += link.face >= 0 && c.faces[link.face].horizontal;
    EXPECT_GT (slabLinks, 0u) << "cells above and below are adjacent";
    for (const auto& link : bt::Access (c).links)
        if (link.face >= 0 && c.faces[link.face].horizontal)
            EXPECT_EQ (c.faces[link.face].kind, bt::FaceClass::Shaft) << "only a stair goes through a slab";
    for (const auto& link : bt::Exterior (c).links)
        EXPECT_FALSE (c.faces[link.face].horizontal);
    // Every cell's solid is closed by its walls and slabs: its faces' plan areas match.
    for (size_t i = 0; i < c.cells.size (); ++i) {
        double top = 0, bottom = 0;
        for (int face : c.cells[i].faces) {
            const auto& f = c.faces[face];
            if (!f.horizontal)
                continue;
            const bool below = f.cells[0] == static_cast<int> (i) &&
                               (f.kind == bt::FaceClass::Slab || f.kind == bt::FaceClass::Shaft ||
                                f.kind == bt::FaceClass::Roof || f.kind == bt::FaceClass::Unknown) &&
                               std::abs (f.z0 - c.cells[i].z1) < 1e-9;
            (below ? top : bottom) += bt::FaceArea (c, face);
        }
        EXPECT_NEAR (top, c.cells[i].area, 1e-3) << "cell " << i;
        EXPECT_NEAR (bottom, c.cells[i].area, 1e-3) << "cell " << i;
    }
    EXPECT_TRUE (bt::Check (c).empty ());
}

// Ids outlive a rebuild: a moved wall keeps both flats; a split or a merge gives new ids that name
// what they came from.
TEST (BuildingTopology, IdsOutliveARebuildAndASplitNamesItsParent)
{
    const auto two = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 5, 4), Rect (5, 0, 10, 4) });
    uint64_t next = 1;
    auto before = bt::Build ({ { 0, 0, 3, &two, {} } });
    bt::Reconcile ({}, before, next);
    EXPECT_EQ (before.units[0].id, 1u);
    EXPECT_EQ (before.units[1].id, 2u);
    EXPECT_EQ (before.cells[0].id, 3u) << "units first, then cells, in order";
    const uint64_t a = before.units[0].id, b = before.units[1].id;
    // A party wall moved by a metre.
    const auto moved = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 6, 4), Rect (6, 0, 10, 4) });
    auto after = bt::Build ({ { 0, 0, 3, &moved, {} } });
    bt::Reconcile (before, after, next);
    EXPECT_EQ (after.units[0].id, a);
    EXPECT_EQ (after.units[1].id, b);
    EXPECT_EQ (after.cells[0].id, before.cells[0].id);
    EXPECT_TRUE (after.units[0].derivedFrom.empty ());
    // The second flat split in two.
    const auto split = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 5, 4), Rect (5, 0, 7.5, 4), Rect (7.5, 0, 10, 4) });
    auto halves = bt::Build ({ { 0, 0, 3, &split, {} } });
    bt::Reconcile (before, halves, next);
    EXPECT_EQ (halves.units[0].id, a);
    for (int u : { 1, 2 }) {
        EXPECT_NE (halves.units[u].id, b);
        EXPECT_EQ (halves.units[u].derivedFrom, (std::vector<uint64_t> { b }));
    }
    EXPECT_NE (halves.units[1].id, halves.units[2].id);
    EXPECT_EQ (bt::UnitById (halves, a), 0);
    EXPECT_EQ (bt::UnitById (halves, b), -1);
    // Both joined.
    const auto one = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 10, 4) });
    auto merged = bt::Build ({ { 0, 0, 3, &one, {} } });
    bt::Reconcile (before, merged, next);
    auto from = merged.units[0].derivedFrom;
    std::sort (from.begin (), from.end ());
    EXPECT_EQ (from, (std::vector<uint64_t> { a, b }));
    // The same flat one story up is another flat.
    auto higher = bt::Build ({ { 1, 3, 3, &two, {} } });
    bt::Reconcile (before, higher, next);
    EXPECT_NE (higher.units[0].id, a);
    EXPECT_TRUE (higher.units[0].derivedFrom.empty ());
}

TEST (BuildingTopology, TheExportIsJsonWithEveryCellFaceAndId)
{
    namespace js = evp::nodegraph::json;
    const auto lower = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 10, 4) });
    const auto upper = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 4, 4), Rect (4, 0, 10, 4) });
    auto c = bt::Build ({ { 0, 0, 3, &lower, {} }, { 1, 3, 3, &upper, {} } });
    uint64_t next = 1;
    bt::Reconcile ({}, c, next);
    const auto parsed = js::Parse (bt::ToJson (c));
    ASSERT_TRUE (parsed.ok) << parsed.error;
    const auto* doc = parsed.value.AsObject ();
    ASSERT_NE (doc, nullptr);
    std::string format;
    ASSERT_TRUE (doc->at ("format").AsString (format));
    EXPECT_EQ (format, "tapioca.cell-complex");
    EXPECT_EQ (doc->at ("cells").AsArray ()->size (), c.cells.size ());
    EXPECT_EQ (doc->at ("faces").AsArray ()->size (), c.faces.size ());
    EXPECT_EQ (doc->at ("flats").AsArray ()->size (), c.units.size ());
    size_t regions = 0;
    for (const auto& face : *doc->at ("faces").AsArray ())
        regions += face.AsObject ()->count ("region");
    EXPECT_EQ (regions, static_cast<size_t> (std::count_if (c.faces.begin (), c.faces.end (),
                                                            [] (const bt::Face& f) { return f.horizontal; })));
    int64_t id = 0;
    ASSERT_TRUE (doc->at ("flats").AsArray ()->front ().AsObject ()->at ("id").AsInteger (id));
    EXPECT_EQ (static_cast<uint64_t> (id), c.units.front ().id);
}

// User, 2026-10-16: "Cell Complex sometimes fail to subtract 0.5m outside wall from cell". Two
// cells lying over each other along the outline share no wall there: the edges run the same way,
// and what is beyond them is still the facade.
TEST (BuildingTopology, CellsOverlappingAlongTheOutlineKeepTheirFacade)
{
    auto s = Flats ({ Rect (0, 0, 10, 4) }, { Rect (0, 0, 10, 4) });
    fs::Core core;
    core.shape = Rect (6, 0, 10, 4); // a stair the flat was laid over
    core.centre = { 8, 2 }, core.width = 4, core.depth = 4;
    s.cores.push_back (core);
    const auto c = bt::Build ({ { 0, 0, 3, &s, {} } });
    for (const auto& f : c.faces)
        if (!f.horizontal && f.beyond == bt::Beyond::Cell)
            EXPECT_FALSE (std::abs (f.a.y - f.b.y) < 1e-9 && (std::abs (f.a.y) < 1e-9 || std::abs (f.a.y - 4) < 1e-9))
                << "no wall between the two along the outline at y = " << f.a.y;
    const auto flat = bt::Clear (c, c.units[0].cells);
    ASSERT_FALSE (flat.empty ());
    for (const auto& ring : flat)
        for (const auto& p : ring) {
            EXPECT_GE (p.y, fp::kFacade - 1e-6) << "the facade wall stands inside the flat";
            EXPECT_LE (p.y, 4 - fp::kFacade + 1e-6);
        }
}
