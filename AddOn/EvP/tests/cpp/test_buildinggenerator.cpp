// The floor plan generator from a whole mass, one floor outline or one unit (BuildingGenerator.hpp):
// every starting point plans with the same generator and gives the same kind of cell complex.
#include "ArchViz/BuildingGenerator.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace bg = geomsrv::archviz::buildinggenerator;
namespace bt = geomsrv::archviz::buildingtopology;
namespace fs = geomsrv::archviz::floorscheme;
namespace fp = geomsrv::archviz::floorprogramme;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
bg::Level LevelAt (int story, double z, std::vector<fs::Ring> outline)
{
    bg::Level level;
    level.story = story, level.z = z, level.height = 3;
    level.outline = std::move (outline);
    return level;
}
double Area (const std::vector<fs::Ring>& rings)
{
    double a = 0;
    for (const auto& r : rings)
        a += fs::Area (r);
    return a;
}
double Length (const bt::Face& f)
{
    return std::hypot (f.b.x - f.a.x, f.b.y - f.a.y);
}
std::string Codes (const std::vector<bt::Diagnostic>& found)
{
    std::string out;
    for (const auto& d : found)
        out += d.code + " ";
    return out;
}
fs::Ring Counter (fs::Ring ring)
{
    if (fs::Area (ring) < 0)
        std::reverse (ring.begin (), ring.end ());
    return ring;
}
// Two rings are the same corners in the same order, from any start.
bool Same (const fs::Ring& a, const fs::Ring& b, double tolerance = 1e-6)
{
    if (a.size () != b.size () || a.empty ())
        return false;
    for (size_t shift = 0; shift < b.size (); ++shift) {
        bool all = true;
        for (size_t i = 0; i < a.size () && all; ++i)
            all =
                std::hypot (a[i].x - b[(i + shift) % b.size ()].x, a[i].y - b[(i + shift) % b.size ()].y) <= tolerance;
        if (all)
            return true;
    }
    return false;
}
// The same rooms, windows and entrance, in the same order.
void ExpectSameRooms (const fs::Flat& want, const fs::Flat& got, const std::string& name)
{
    ASSERT_EQ (got.roomList.size (), want.roomList.size ()) << name;
    for (size_t i = 0; i < want.roomList.size (); ++i) {
        EXPECT_EQ (got.roomList[i].kind, want.roomList[i].kind) << name << " room " << i;
        EXPECT_TRUE (Same (Counter (got.roomList[i].shape), Counter (want.roomList[i].shape))) << name << " room " << i;
    }
    ASSERT_EQ (got.windows.size (), want.windows.size ()) << name;
    for (size_t i = 0; i < want.windows.size (); ++i)
        EXPECT_TRUE (Same ({ got.windows[i].a, got.windows[i].b }, { want.windows[i].a, want.windows[i].b }))
            << name << " window " << i;
    EXPECT_TRUE (Same ({ got.door.a, got.door.b }, { want.door.a, want.door.b })) << name << " door";
}
// A flat as its band divided it: a rectangle the mould did not grow.
bool AsDivided (const fs::Flat& f)
{
    return !f.manual && f.shape.size () == 4 && std::abs (f.gross - f.frontage * f.depth) < 1e-6;
}
} // namespace

// User, 2026-10-10: "stair core must be same position from ground to top floor".
TEST (BuildingGenerator, AMassHoldsOneStairStackOnEveryLevel)
{
    // Given out of order; the top level steps back to 40 m.
    const auto g = bg::PlanMass ({ LevelAt (2, 6, { Rect (0, 0, 40, 16) }), LevelAt (0, 0, { Rect (0, 0, 60, 16) }),
                                   LevelAt (1, 3, { Rect (0, 0, 60, 16) }) },
                                 fp::Default ());
    ASSERT_EQ (g.levels.size (), 3u);
    for (size_t i = 0; i < 3; ++i)
        EXPECT_EQ (g.levels[i].story, static_cast<int> (i)) << "lowest first";
    ASSERT_FALSE (g.stack.empty ());
    EXPECT_TRUE (g.diagnostics.empty ()) << Codes (g.diagnostics);
    for (const auto& core : g.stack)
        EXPECT_LE (core.centre.x, 40) << "planned on the floor every level has";
    for (size_t i = 0; i < g.schemes.size (); ++i) {
        const auto& s = g.schemes[i];
        EXPECT_TRUE (s.ok) << "level " << i;
        ASSERT_EQ (s.cores.size (), g.stack.size ()) << "level " << i;
        for (size_t k = 0; k < s.cores.size (); ++k) {
            EXPECT_NEAR (s.cores[k].centre.x, g.stack[k].centre.x, 1e-6) << "level " << i;
            EXPECT_NEAR (s.cores[k].centre.y, g.stack[k].centre.y, 1e-6) << "level " << i;
        }
        double cells = 0;
        for (int id : g.complex.floors[i].cells)
            cells += g.complex.cells[id].area;
        EXPECT_NEAR (cells, Area (s.outline), 0.05) << "level " << i << ": cells tile it";
    }
    std::map<int, std::vector<int>> stairs; // stack stair -> floors
    for (const auto& cell : g.complex.cells)
        if (cell.kind == bt::CellKind::Core) {
            ASSERT_GE (cell.stack, 0);
            stairs[cell.stack].push_back (cell.floor);
        }
    ASSERT_EQ (stairs.size (), g.stack.size ());
    for (const auto& [stair, floors] : stairs)
        EXPECT_EQ (floors, (std::vector<int> { 0, 1, 2 })) << "stair " << stair;
    EXPECT_TRUE (bt::Check (g.complex).empty ()) << Codes (bt::Check (g.complex));
}

TEST (BuildingGenerator, AFloorOutlineIsABuildingOfOneFloor)
{
    const auto g = bg::PlanFloor (LevelAt (0, 0, { Rect (0, 0, 48, 16), Rect (0, 16, 16, 48) }), fp::Default ());
    ASSERT_EQ (g.complex.floors.size (), 1u);
    ASSERT_TRUE (g.schemes.front ().ok);
    EXPECT_EQ (g.complex.units.size (), g.schemes.front ().flats.size ());
    EXPECT_EQ (g.stack.size (), g.schemes.front ().cores.size ()) << "its own stairs are its stack";
    for (const auto& cell : g.complex.cells)
        if (cell.kind == bt::CellKind::Core)
            EXPECT_GE (cell.stack, 0);
    EXPECT_TRUE (bt::Check (g.complex).empty ()) << Codes (bt::Check (g.complex));
}

// A flat on its own: a corridor below it, flats either side, open air above.
TEST (BuildingGenerator, AUnitOnItsOwnReadsWhatLiesAroundIt)
{
    auto level = LevelAt (0, 0, { Rect (0, 0, 6.6, 7.5) });
    level.context = { { bt::CellKind::Corridor, Rect (-6.6, -1.8, 13.2, 0) },
                      { bt::CellKind::Flat, Rect (-6.6, 0, 0, 7.5) },
                      { bt::CellKind::Flat, Rect (6.6, 0, 13.2, 7.5) } };
    const auto programme = fp::Default ();
    const size_t twoRooms = 1;
    ASSERT_EQ (programme.types[twoRooms].rooms, 2);
    const auto g = bg::PlanUnit (level, programme, twoRooms);
    EXPECT_TRUE (g.diagnostics.empty ()) << Codes (g.diagnostics);
    ASSERT_EQ (g.complex.units.size (), 1u);
    const auto& unit = g.complex.units.front ();
    EXPECT_TRUE (unit.roomsKnown);
    std::map<fs::RoomKind, int> rooms;
    for (int id : unit.cells)
        if (g.complex.cells[id].kind == bt::CellKind::Room)
            ++rooms[g.complex.cells[id].room];
    EXPECT_EQ (rooms[fs::RoomKind::Living], 1);
    EXPECT_EQ (rooms[fs::RoomKind::Bedroom], 1);
    EXPECT_EQ (rooms[fs::RoomKind::Hall], 1);
    EXPECT_GE (rooms[fs::RoomKind::Bath], 1);
    // Walls: facade on top only, the corridor wall below, party walls to the flats either side;
    // nothing between or beyond the context cells.
    std::map<bt::FaceClass, double> walls;
    for (const auto& f : g.complex.faces) {
        walls[f.kind] += Length (f);
        EXPECT_FALSE (g.complex.cells[f.cells[0]].context) << "a face is the unit's";
    }
    EXPECT_NEAR (walls[bt::FaceClass::Facade], 6.6, 1e-6);
    EXPECT_NEAR (walls[bt::FaceClass::UnitCorridor], 6.6, 1e-6);
    EXPECT_NEAR (walls[bt::FaceClass::UnitParty], 15, 1e-6);
    // The door on the corridor wall, the windows on the facade.
    size_t doors = 0, windows = 0;
    for (const auto& a : g.complex.apertures) {
        const bool door = a.kind == bt::ApertureKind::Door;
        (door ? doors : windows) += 1;
        EXPECT_EQ (g.complex.faces[a.face].kind, door ? bt::FaceClass::UnitCorridor : bt::FaceClass::Facade);
    }
    EXPECT_EQ (doors, 1u);
    EXPECT_EQ (windows, 2u) << "the living room and the bedroom";
    // The corridor around it leads to a stair beyond: every rule holds.
    EXPECT_TRUE (bt::Check (g.complex).empty ()) << Codes (bt::Check (g.complex));
    const auto figures = bt::Figures (g.complex, 0);
    EXPECT_TRUE (figures.entrance && figures.reachesStair);
    EXPECT_NEAR (figures.clearArea, (6.6 - 0.2) * (7.5 - 0.6), 1e-3) << "0.5 m facade, 0.1 m of each partition";
    EXPECT_NEAR (g.schemes.front ().flats.front ().net, figures.clearArea, 1e-6);
    EXPECT_TRUE (g.schemes.front ().flats.front ().inRange) << "44 m2 is a 2R";
    // Context is never counted: the floor is the unit.
    double cells = 0;
    for (int id : g.complex.floors.front ().cells)
        cells += g.complex.cells[id].area;
    EXPECT_NEAR (cells, 6.6 * 7.5, 1e-4) << "within the clipping precision";
    EXPECT_EQ (g.complex.floors.front ().context.size (), 3u);
}

TEST (BuildingGenerator, AUnitWithoutCirculationHasNoEntrance)
{
    auto level = LevelAt (0, 0, { Rect (0, 0, 6.6, 7.5) });
    level.context = { { bt::CellKind::Flat, Rect (0, -7.5, 6.6, 0) } };
    const auto g = bg::PlanUnit (level, fp::Default (), 1);
    ASSERT_EQ (g.complex.units.size (), 1u);
    EXPECT_TRUE (g.complex.units.front ().roomsKnown) << "rooms laid; only the door has nowhere to open";
    const auto& flat = g.schemes.front ().flats.front ();
    EXPECT_LT (std::hypot (flat.door.b.x - flat.door.a.x, flat.door.b.y - flat.door.a.y), 1e-9);
    const auto found = bt::Check (g.complex);
    EXPECT_TRUE (std::any_of (found.begin (), found.end (), [] (const bt::Diagnostic& d) {
        return d.code == "unit.no_entrance";
    })) << Codes (found);
}

TEST (BuildingGenerator, AUnitWithoutFacadeLeavesItsRoomsToTheUser)
{
    // An L walled in all round by other flats.
    auto level = LevelAt (0, 0, { { { 0, 0 }, { 10, 0 }, { 10, 4 }, { 4, 4 }, { 4, 10 }, { 0, 10 } } });
    level.context = { { bt::CellKind::Flat, Rect (-5, -5, 15, 0) },
                      { bt::CellKind::Flat, Rect (-5, 0, 0, 15) },
                      { bt::CellKind::Flat, Rect (10, 0, 15, 15) },
                      { bt::CellKind::Flat, Rect (0, 10, 10, 15) },
                      { bt::CellKind::Flat, Rect (4, 4, 10, 10) } };
    const auto g = bg::PlanUnit (level, fp::Default (), 2);
    ASSERT_EQ (g.complex.units.size (), 1u);
    EXPECT_FALSE (g.complex.units.front ().roomsKnown);
    EXPECT_TRUE (g.schemes.front ().flats.front ().manual);
    ASSERT_EQ (g.diagnostics.size (), 1u);
    EXPECT_EQ (g.diagnostics.front ().code, "unit.rooms_manual");
    // Planned again, still one report.
    auto again = g;
    EXPECT_FALSE (bg::ReplanUnit (again, 0));
    EXPECT_EQ (again.diagnostics.size (), 1u);
}

namespace {
// The unit's cells: they cover it once, and how much of it its rooms take.
struct Tiling {
    double cells = 0, rooms = 0, rest = 0;
    double lowest = 1e18; // of any room cell
};
Tiling Tile (const bt::Complex& c, int unit)
{
    Tiling t;
    for (int id : c.units[unit].cells) {
        const auto& cell = c.cells[id];
        t.cells += cell.area;
        (cell.kind == bt::CellKind::Room ? t.rooms : t.rest) += cell.area;
        if (cell.kind == bt::CellKind::Room)
            for (const auto& p : cell.shape)
                t.lowest = (std::min) (t.lowest, p.y);
    }
    return t;
}
} // namespace

// User, 2026-10-14: non-rectangular units. An L: its rooms in the bar, the arm taken by the room
// beside it, walls produced straight.
TEST (BuildingGenerator, AnLShapedUnitGrowsARoomIntoItsArm)
{
    const fs::Ring l { { 0, -3 }, { 4, -3 }, { 4, 0 }, { 9.9, 0 }, { 9.9, 7.5 }, { 0, 7.5 } };
    auto level = LevelAt (0, 0, { l });
    level.context = { { bt::CellKind::Corridor, Rect (4, -1.8, 20, 0) },
                      { bt::CellKind::Flat, Rect (9.9, 0, 16, 7.5) } };
    const size_t threeRooms = 3; // 65-75 m2
    ASSERT_EQ (fp::Default ().types[threeRooms].rooms, 3);
    const auto g = bg::PlanUnit (level, fp::Default (), threeRooms);
    EXPECT_TRUE (g.diagnostics.empty ()) << Codes (g.diagnostics);
    ASSERT_EQ (g.complex.units.size (), 1u);
    EXPECT_TRUE (g.complex.units.front ().roomsKnown);
    EXPECT_FALSE (g.schemes.front ().flats.front ().manual);
    const auto t = Tile (g.complex, 0);
    EXPECT_NEAR (t.cells, std::abs (fs::Area (l)), 0.05) << "its cells tile it";
    EXPECT_LT (t.lowest, -2.9) << "a room reaches the end of the arm";
    EXPECT_LT (t.rest, 0.1 * t.cells) << "little is left over";
    // The arm is part of a room, not a room of its own: a 3R keeps one living room and two
    // bedrooms, one of them L-shaped.
    std::map<fs::RoomKind, int> rooms;
    bool shaped = false;
    for (int id : g.complex.units.front ().cells) {
        const auto& cell = g.complex.cells[id];
        if (cell.kind == bt::CellKind::Room)
            ++rooms[cell.room], shaped = shaped || cell.shape.size () > 4;
    }
    EXPECT_EQ (rooms[fs::RoomKind::Living], 1);
    EXPECT_EQ (rooms[fs::RoomKind::Bedroom], 2);
    EXPECT_TRUE (shaped);
    EXPECT_TRUE (bt::Check (g.complex).empty ()) << Codes (bt::Check (g.complex));
}

// A slanted end: the rooms stay rectangles, the wedge is the flat's rest.
TEST (BuildingGenerator, ASlantedEndIsLeftToTheFlatsRest)
{
    const fs::Ring trapezoid { { 0, 0 }, { 8, 0 }, { 9.5, 7.5 }, { 0, 7.5 } };
    auto level = LevelAt (0, 0, { trapezoid });
    level.context = { { bt::CellKind::Corridor, Rect (-5, -1.8, 15, 0) } };
    const auto g = bg::PlanUnit (level, fp::Default (), 3);
    ASSERT_EQ (g.complex.units.size (), 1u);
    EXPECT_TRUE (g.complex.units.front ().roomsKnown);
    const auto t = Tile (g.complex, 0);
    EXPECT_NEAR (t.cells, std::abs (fs::Area (trapezoid)), 0.05);
    EXPECT_GE (t.rest, 1.5 * 7.5 / 2 - 0.05) << "the wedge";
    EXPECT_LE (t.rest, 1.5 * 7.5 / 2 + 0.1 * 7.5 + 0.05) << "and at most the wall strip beside it";
    for (const auto& room : g.schemes.front ().flats.front ().roomList)
        EXPECT_EQ (room.shape.size (), 4u);
    EXPECT_TRUE (bt::Check (g.complex).empty ()) << Codes (bt::Check (g.complex));
}

// Replanning a flat from the walls the complex gives it is the generator's own room plan.
TEST (BuildingGenerator, ReplanningAUnitInItsFloorGivesTheGeneratorsRooms)
{
    fs::Ring court = Rect (14, 14, 36, 36);
    std::reverse (court.begin (), court.end ());
    for (const auto& outline : { std::vector<fs::Ring> { Rect (0, 0, 60, 16) },
                                 { Rect (0, 0, 48, 12) },
                                 { Rect (0, 0, 48, 16), Rect (0, 16, 16, 48) },
                                 { Rect (0, 0, 50, 50), court },
                                 { Rect (0, 0, 60, 16), Rect (22, 16, 38, 40) } }) {
        auto g = bg::PlanFloor (LevelAt (0, 0, outline), fp::Default ());
        const auto before = g.schemes.front ();
        const size_t cells = g.complex.cells.size (), faces = g.complex.faces.size ();
        size_t compared = 0;
        for (size_t u = 0; u < g.complex.units.size (); ++u) {
            const auto& flat = before.flats[g.complex.units[u].flat];
            if (!AsDivided (flat))
                continue;
            ASSERT_TRUE (bg::ReplanUnit (g, static_cast<int> (u))) << u;
            ExpectSameRooms (flat, g.schemes.front ().flats[g.complex.units[u].flat], "flat " + std::to_string (u));
            ++compared;
        }
        EXPECT_GT (compared, 0u);
        EXPECT_EQ (g.complex.cells.size (), cells) << "the same complex";
        EXPECT_EQ (g.complex.faces.size (), faces);
    }
}

// The same flat from either starting point: its floor, or on its own among the rest of that floor.
TEST (BuildingGenerator, AUnitOnItsOwnAmongItsFloorIsThatUnit)
{
    const auto g = bg::PlanFloor (LevelAt (0, 0, { Rect (0, 0, 60, 16) }), fp::Default ());
    size_t compared = 0;
    for (size_t u = 0; u < g.complex.units.size (); ++u) {
        const auto& flat = g.schemes.front ().flats[g.complex.units[u].flat];
        if (!AsDivided (flat))
            continue;
        auto level = LevelAt (0, 0, { flat.shape });
        level.context = bt::Around (g.complex, static_cast<int> (u));
        const auto one = bg::PlanUnit (level, fp::Default (), flat.type, flat.axis);
        ASSERT_EQ (one.complex.units.size (), 1u);
        ExpectSameRooms (flat, one.schemes.front ().flats.front (), "flat " + std::to_string (u));
        EXPECT_TRUE (bt::Check (one.complex).empty ()) << u << ": " << Codes (bt::Check (one.complex));
        ++compared;
    }
    EXPECT_GT (compared, 0u);
}

TEST (BuildingGenerator, RetypingAUnitReplansOnlyIt)
{
    const auto programme = fp::Default ();
    auto g = bg::PlanFloor (LevelAt (0, 0, { Rect (0, 0, 60, 16) }), programme);
    // A flat of two rooms or more becomes the programme's smallest type.
    int unit = -1;
    for (size_t u = 0; u < g.complex.units.size () && unit < 0; ++u)
        if (g.complex.units[u].roomsKnown && g.complex.units[u].rooms >= 2 &&
            AsDivided (g.schemes.front ().flats[g.complex.units[u].flat]))
            unit = static_cast<int> (u);
    ASSERT_GE (unit, 0);
    const auto before = g.schemes.front ();
    const size_t from = before.flats[g.complex.units[unit].flat].type;
    ASSERT_NE (from, 0u);
    ASSERT_TRUE (bg::RetypeUnit (g, unit, programme, 0));
    const auto& after = g.schemes.front ();
    const auto& flat = after.flats[g.complex.units[unit].flat];
    EXPECT_EQ (flat.type, 0u);
    EXPECT_EQ (flat.rooms, programme.types[0].rooms);
    EXPECT_EQ (after.counts[from], before.counts[from] - 1);
    EXPECT_EQ (after.counts[0], before.counts[0] + 1);
    EXPECT_TRUE (std::any_of (flat.roomList.begin (), flat.roomList.end (), [] (const fs::Room& r) {
        return r.kind == fs::RoomKind::Alcove;
    })) << "a 1.5R sleeps in an alcove";
    for (size_t i = 0; i < after.flats.size (); ++i)
        if (static_cast<int> (i) != g.complex.units[unit].flat)
            ExpectSameRooms (before.flats[i], after.flats[i], "untouched flat " + std::to_string (i));
}

TEST (BuildingGenerator, ReplanningALevelHoldsTheStack)
{
    const auto programme = fp::Default ();
    auto g =
        bg::PlanMass ({ LevelAt (0, 0, { Rect (0, 0, 60, 16) }), LevelAt (1, 3, { Rect (0, 0, 40, 16) }) }, programme);
    const auto stack = g.stack;
    const auto ground = g.schemes[0];
    // The upper level grows back to the full bar: replanned, it still stands on the same stairs.
    g.levels[1].outline = { Rect (0, 0, 60, 16) };
    ASSERT_TRUE (bg::ReplanFloor (g, 1, programme));
    EXPECT_EQ (g.stack, stack) << "a level receives the stack, never decides it";
    ASSERT_EQ (g.schemes[1].cores.size (), stack.size ());
    for (size_t k = 0; k < stack.size (); ++k)
        EXPECT_NEAR (std::hypot (g.schemes[1].cores[k].centre.x - stack[k].centre.x,
                                 g.schemes[1].cores[k].centre.y - stack[k].centre.y),
                     0, 1e-6);
    EXPECT_NEAR (g.schemes[1].gross, 60 * 16, 0.05);
    ASSERT_EQ (g.schemes[0].flats.size (), ground.flats.size ()) << "the other level is not planned again";
    for (size_t i = 0; i < ground.flats.size (); ++i)
        EXPECT_TRUE (Same (Counter (g.schemes[0].flats[i].shape), Counter (ground.flats[i].shape)));
    EXPECT_TRUE (bt::Check (g.complex).empty ()) << Codes (bt::Check (g.complex));
}

TEST (BuildingGenerator, NothingToPlanIsTold)
{
    EXPECT_TRUE (bg::PlanMass ({}, fp::Default ()).complex.floors.empty ());
    const auto g = bg::PlanUnit (LevelAt (0, 0, {}), fp::Default (), 0);
    EXPECT_TRUE (g.complex.units.empty ());
    ASSERT_FALSE (g.diagnostics.empty ());
    EXPECT_EQ (g.diagnostics.back ().code, "input.empty");
    auto copy = g;
    EXPECT_FALSE (bg::ReplanUnit (copy, 0));
    EXPECT_FALSE (bg::ReplanFloor (copy, 3, fp::Default ()));
    EXPECT_FALSE (bg::RetypeUnit (copy, 0, fp::Default (), 0));
}

// Stable ids (handoff 10.7): a part planned again leaves every other id as it was, and the flat
// it changed keeps its own; its rooms are found again or name the rooms they replace.
TEST (BuildingGenerator, ReplanningAndRetypingKeepTheIds)
{
    const auto programme = fp::Default ();
    auto g = bg::PlanFloor (LevelAt (0, 0, { Rect (0, 0, 60, 16) }), programme);
    auto ids = [] (const bt::Complex& c) {
        std::vector<uint64_t> units, cells;
        for (const auto& u : c.units)
            units.push_back (u.id);
        for (const auto& cell : c.cells)
            cells.push_back (cell.id);
        return std::make_pair (units, cells);
    };
    const auto first = ids (g.complex);
    for (uint64_t id : first.first)
        EXPECT_NE (id, 0u);
    int unit = -1;
    for (size_t u = 0; u < g.complex.units.size () && unit < 0; ++u)
        if (g.complex.units[u].roomsKnown && g.complex.units[u].rooms >= 2 &&
            AsDivided (g.schemes.front ().flats[g.complex.units[u].flat]))
            unit = static_cast<int> (u);
    ASSERT_GE (unit, 0);
    ASSERT_TRUE (bg::ReplanUnit (g, unit));
    EXPECT_EQ (ids (g.complex), first) << "the same rooms: the same ids";
    const uint64_t flat = g.complex.units[unit].id;
    std::vector<uint64_t> others;
    for (const auto& cell : g.complex.cells)
        if (cell.unit != unit)
            others.push_back (cell.id);
    ASSERT_TRUE (bg::RetypeUnit (g, unit, programme, 0));
    EXPECT_EQ (g.complex.units[unit].id, flat) << "a retyped flat is the same flat";
    std::vector<uint64_t> after;
    for (const auto& cell : g.complex.cells)
        if (cell.unit != unit)
            after.push_back (cell.id);
    EXPECT_EQ (after, others);
    for (int id : g.complex.units[unit].cells) {
        const auto& cell = g.complex.cells[id];
        const bool kept = std::find (first.second.begin (), first.second.end (), cell.id) != first.second.end ();
        EXPECT_TRUE (kept || !cell.derivedFrom.empty ()) << "a new room names the rooms it replaces";
    }
}

// User, 2026-10-14: "Levels should come from archicad project story settings not other way around."
TEST (BuildingGenerator, LevelsAreTheProjectsStories)
{
    // Stories out of order; the mass from 0 to 12 m, set back above 6 m.
    const std::vector<bg::Story> stories { { 2, 6.5 }, { 0, 0 }, { 4, 13 }, { 1, 3 }, { 3, 10 } };
    std::vector<double> asked;
    auto cut = [&] (double z) {
        asked.push_back (z);
        return std::vector<fs::Ring> { z < 6 ? Rect (0, 0, 60, 16) : Rect (0, 0, 40, 16) };
    };
    const auto levels = bg::StoryLevels (stories, 0, 12, cut);
    ASSERT_EQ (levels.size (), 3u) << "the 2 m storey from 10 to 12 m is no level";
    const std::vector<std::tuple<int, double, double>> want { { 0, 0, 3 }, { 1, 3, 3.5 }, { 2, 6.5, 3.5 } };
    for (size_t i = 0; i < want.size (); ++i) {
        EXPECT_EQ (levels[i].story, std::get<0> (want[i]));
        EXPECT_NEAR (levels[i].z, std::get<1> (want[i]), 1e-9);
        EXPECT_NEAR (levels[i].height, std::get<2> (want[i]), 1e-9);
    }
    EXPECT_NEAR (Area (levels[2].outline), 40 * 16, 1e-9) << "cut at the story's level";
    // A mass standing between two story levels starts in the story below it.
    const auto raised = bg::StoryLevels (stories, 0.5, 9, cut);
    ASSERT_FALSE (raised.empty ());
    EXPECT_EQ (raised.front ().story, 0);
    EXPECT_NEAR (raised.front ().z, 0.5, 1e-9);
    EXPECT_NEAR (raised.front ().height, 2.5, 1e-9);
    EXPECT_EQ (bg::StoryLevels (stories, 1, 9, cut).front ().story, 1) << "a 2 m first storey is no level";
    // Where the mass has no floor, no level.
    const auto holed = bg::StoryLevels (stories, 0, 12, [] (double z) {
        return z == 3 ? std::vector<fs::Ring> {} : std::vector<fs::Ring> { Rect (0, 0, 60, 16) };
    });
    EXPECT_EQ (holed.size (), 2u);
    // And the levels plan as a mass: one stack inside the setback.
    const auto g = bg::PlanMass (levels, fp::Default ());
    EXPECT_EQ (g.complex.floors.size (), 3u);
    EXPECT_TRUE (bt::Check (g.complex).empty ()) << Codes (bt::Check (g.complex));
}

TEST (BuildingGenerator, TheSameMassPlannedTwiceHasTheSameIds)
{
    const std::vector<bg::Level> levels { LevelAt (0, 0, { Rect (0, 0, 60, 16) }),
                                          LevelAt (1, 3, { Rect (0, 0, 40, 16) }) };
    const auto a = bg::PlanMass (levels, fp::Default ());
    const auto b = bg::PlanMass (levels, fp::Default ());
    ASSERT_EQ (a.complex.cells.size (), b.complex.cells.size ());
    for (size_t i = 0; i < a.complex.cells.size (); ++i)
        EXPECT_EQ (a.complex.cells[i].id, b.complex.cells[i].id);
    EXPECT_EQ (a.nextId, b.nextId);
}
