// A flat's entrance as the user's (user, 2026-11-15: "graph editor should show also unit rooms and
// allow to change entrance door location"): the door moves along its wall to the corridor, the
// hall follows it, the design keeps it; and the access graph shows each flat's rooms.
#include "ArchViz/FloorSchemeDesigns.hpp"
#include "ArchViz/FloorSchemeSketch.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

namespace fs = geomsrv::archviz::floorscheme;
namespace fe = geomsrv::archviz::floorscheme::edit;
namespace fp = geomsrv::archviz::floorprogramme;
namespace sk = geomsrv::archviz::floorscheme::sketch;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
double Far (fs::Vec a, fs::Vec b)
{
    return std::hypot (a.x - b.x, a.y - b.y);
}
struct Bar {
    std::vector<fs::Ring> outline { Rect (0, 0, 60, 16) };
    fs::Scheme scheme = fs::Generate (outline, fp::Default ());
    // A flat in a row along the corridor wide enough for its door to go somewhere: the widest.
    int Wide () const
    {
        int best = -1;
        for (size_t i = 0; i < scheme.flats.size (); ++i) {
            const auto& f = scheme.flats[i];
            if (f.band >= 0 && !f.cap && !f.manual && fe::DoorOf (scheme, int (i)) &&
                (best < 0 || f.frontage > scheme.flats[best].frontage))
                best = int (i);
        }
        return best;
    }
};
size_t Errors (const fs::Scheme& s)
{
    return std::count_if (s.diagnostics.begin (), s.diagnostics.end (),
                          [] (const fs::Diagnostic& d) { return d.level == fs::Diagnostic::Error; });
}
} // namespace

TEST (FloorSchemeDoor, AnEntranceMovesAlongItsWallAndTheHallFollowsIt)
{
    Bar bar;
    const int flat = bar.Wide ();
    ASSERT_GE (flat, 0);
    const auto& f = bar.scheme.flats[flat];
    const fs::Vec was = *fe::DoorOf (bar.scheme, flat);
    // Towards the end of the flat's corridor wall that is farther from the door.
    const fs::Vec along = f.axis;
    const fs::Vec mid = fe::FlatAt (bar.scheme, was) >= 0 ? was : was;
    double lo = 1e18, hi = -1e18;
    for (const auto& p : f.shape) {
        const double u = (p.x - was.x) * along.x + (p.y - was.y) * along.y;
        lo = (std::min) (lo, u), hi = (std::max) (hi, u);
    }
    const double reach = -lo > hi ? lo : hi;
    ASSERT_GT (std::abs (reach), 1.5) << "the flat is wide enough for the door to move";
    const fs::Vec to { mid.x + along.x * reach, mid.y + along.y * reach };
    std::string why;
    const auto design = fe::MoveDoor (bar.scheme, {}, flat, to, &why);
    ASSERT_EQ (design.pins.doors.size (), 1u) << why;
    const auto next = fe::Run (bar.outline, fp::Default (), design);
    EXPECT_EQ (Errors (next), Errors (bar.scheme));
    EXPECT_EQ (next.flats.size (), bar.scheme.flats.size ());
    const int again = fe::FlatAt (next, design.pins.doors[0].at);
    ASSERT_GE (again, 0);
    const auto now = fe::DoorOf (next, again);
    ASSERT_TRUE (now.has_value ());
    EXPECT_GT (Far (*now, was), 0.5) << "the door moved";
    EXPECT_LT (Far (*now, to), Far (was, to)) << "towards where it was put";
    // The door is in the hall's wall.
    bool hall = false;
    for (const auto& r : next.flats[again].roomList)
        if (r.kind == fs::RoomKind::Hall) {
            double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
            for (const auto& p : r.shape)
                x0 = (std::min) (x0, p.x), y0 = (std::min) (y0, p.y), x1 = (std::max) (x1, p.x),
                y1 = (std::max) (y1, p.y);
            hall = hall || (now->x > x0 - 0.3 && now->x < x1 + 0.3 && now->y > y0 - 0.3 && now->y < y1 + 0.3);
        }
    EXPECT_TRUE (hall);
    // Every other flat's door is where it was.
    for (size_t i = 0; i < bar.scheme.flats.size (); ++i)
        if (int (i) != flat)
            if (const auto before = fe::DoorOf (bar.scheme, int (i))) {
                const int same =
                    fe::FlatAt (next, { (bar.scheme.flats[i].shape[0].x + bar.scheme.flats[i].shape[2].x) / 2,
                                        (bar.scheme.flats[i].shape[0].y + bar.scheme.flats[i].shape[2].y) / 2 });
                if (const auto after = same >= 0 ? fe::DoorOf (next, same) : std::nullopt)
                    EXPECT_LT (Far (*before, *after), 1e-6) << "flat " << i;
            }
    // Given back, the generator's door returns.
    const auto reset = fe::Run (bar.outline, fp::Default (), fe::ResetDoor (next, design, again));
    EXPECT_LT (Far (*fe::DoorOf (reset, fe::FlatAt (reset, design.pins.doors[0].at)), was), 1e-6);
}

TEST (FloorSchemeDoor, AnEntranceStaysOnCirculationAndARefusalSaysWhy)
{
    Bar bar;
    const int flat = bar.Wide ();
    ASSERT_GE (flat, 0);
    const fs::Vec was = *fe::DoorOf (bar.scheme, flat);
    // Asked for far past the flat: the nearest place on its own wall that opens onto circulation.
    const auto& f = bar.scheme.flats[flat];
    const auto place = fe::DoorPlace (bar.scheme, flat, { was.x + f.axis.x * 100, was.y + f.axis.y * 100 });
    ASSERT_TRUE (place.has_value ());
    const fs::Vec across { -f.axis.y, f.axis.x };
    EXPECT_NEAR ((place->x - was.x) * across.x + (place->y - was.y) * across.y, 0, 1e-6) << "on the same wall";
    bool on = false;
    for (const auto& p : f.shape)
        on = on || Far (p, *place) < f.frontage + 1e-6;
    EXPECT_TRUE (on) << "within the flat's own wall";
    // Where it is already: nothing changes, and it is said.
    std::string why;
    const auto same = fe::MoveDoor (bar.scheme, {}, flat, was, &why);
    EXPECT_TRUE (same.pins.doors.empty ());
    EXPECT_FALSE (why.empty ());
}

TEST (FloorSchemeDoor, ADesignKeepsItsEntrances)
{
    fe::Designs designs;
    designs.floors[2].pins.doors.push_back ({ { 12.5, 7.25 } });
    fe::Designs read;
    std::string error;
    ASSERT_TRUE (fe::FromJson (fe::ToJson (designs), read, error)) << error;
    ASSERT_EQ (read.floors[2].pins.doors.size (), 1u);
    EXPECT_NEAR (read.floors[2].pins.doors[0].at.x, 12.5, 1e-9);
    EXPECT_NEAR (read.floors[2].pins.doors[0].at.y, 7.25, 1e-9);
}

TEST (FloorSchemeDoor, TheGraphShowsEveryFlatsRoomsEnteredFromItsHall)
{
    Bar bar;
    const auto g = sk::Access (bar.scheme);
    for (size_t i = 0; i < bar.scheme.flats.size (); ++i) {
        const auto& f = bar.scheme.flats[i];
        size_t rooms = 0, halls = 0;
        for (const auto& r : f.roomList)
            rooms += r.extends < 0, halls += r.extends < 0 && r.kind == fs::RoomKind::Hall;
        const auto nodes = std::count_if (g.nodes.begin (), g.nodes.end (), [&] (const sk::Graph::Node& n) {
            return n.kind == sk::Graph::Kind::Room && n.index == int (i);
        });
        EXPECT_EQ (size_t (nodes), rooms) << "flat " << i;
        if (!halls)
            continue;
        // A tree: the door to the hall, and each other room from one before it.
        const auto inside = std::count_if (g.edges.begin (), g.edges.end (), [&] (const sk::Graph::Edge& e) {
            return e.kind == sk::Graph::Edge::Kind::Inside && e.flat == int (i);
        });
        EXPECT_EQ (size_t (inside), rooms - 1 + (fe::DoorOf (bar.scheme, int (i)) ? 1 : 0)) << "flat " << i;
        // Every room's node is the end of one of them.
        for (const auto& n : g.nodes)
            if (n.kind == sk::Graph::Kind::Room && n.index == int (i))
                EXPECT_TRUE (std::any_of (g.edges.begin (), g.edges.end (),
                                          [&] (const sk::Graph::Edge& e) {
                                              return e.kind == sk::Graph::Edge::Kind::Inside && e.flat == int (i) &&
                                                     Far (e.line.back (), n.at) < 1e-9;
                                          }))
                    << "flat " << i << " room " << n.room;
    }
}
