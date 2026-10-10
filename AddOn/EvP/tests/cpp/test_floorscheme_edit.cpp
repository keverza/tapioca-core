#include "ArchViz/FloorSchemeEdit.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <sstream>

namespace fs = geomsrv::archviz::floorscheme;
namespace fe = geomsrv::archviz::floorscheme::edit;
namespace fp = geomsrv::archviz::floorprogramme;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
size_t Errors (const fs::Scheme& s)
{
    size_t n = 0;
    for (const auto& d : s.diagnostics)
        n += d.level == fs::Diagnostic::Error;
    return n;
}
std::string Report (const fs::Scheme& s)
{
    std::ostringstream out;
    out << s.flats.size () << " flats, " << s.cores.size () << " stairs\n";
    for (const auto& d : s.diagnostics)
        if (d.level == fs::Diagnostic::Error)
            out << "  " << d.code << " " << d.text << " at " << d.at.x << "," << d.at.y << "\n";
    return out.str ();
}
fs::Vec Mid (const fs::Ring& r)
{
    fs::Vec c;
    for (const auto& p : r)
        c.x += p.x / r.size (), c.y += p.y / r.size ();
    return c;
}
// A session on one floor, run once.
struct Floor {
    fe::Session session;
    fs::Scheme scheme;
    explicit Floor (std::vector<fs::Ring> floor)
    {
        session.floor = std::move (floor);
        scheme = fe::Run (session.floor, session.programme, session.design);
    }
    const fs::Scheme& Apply (fe::Design next)
    {
        session.Commit (std::move (next));
        scheme = fe::Run (session.floor, session.programme, session.design);
        return scheme;
    }
};
} // namespace

TEST (FloorSchemeEdit, RectanglesAddToAndCutFromTheSilhouette)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    const double gross = f.scheme.gross;
    f.Apply (fe::AddRect (f.session.design, fe::Rectangle (f.scheme, { 60, 0 }, { 72, 16 })));
    EXPECT_NEAR (f.scheme.gross, gross + 12 * 16, 1.0);
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    f.Apply (fe::CutRect (f.session.design, fe::Rectangle (f.scheme, { 0, 0 }, { 6, 6 })));
    EXPECT_NEAR (f.scheme.gross, gross + 12 * 16 - 36, 1.0);
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
}

TEST (FloorSchemeEdit, StairsMoveAddAndGoDownToTheAreaMinimum)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    ASSERT_EQ (f.scheme.cores.size (), 2u);
    const fs::Vec from = f.scheme.cores[0].centre, to { from.x + 2.0, from.y };
    f.Apply (fe::MoveStair (f.scheme, f.session.design, 0, to));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    double near = 1e9;
    for (const auto& c : f.scheme.cores)
        near = (std::min) (near, std::abs (c.centre.x - to.x));
    EXPECT_LT (near, 0.6);
    f.Apply (fe::AddStair (f.scheme, f.session.design, { 30, 12 }));
    EXPECT_EQ (f.scheme.cores.size (), 3u) << Report (f.scheme);
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    f.Apply (fe::RemoveStair (f.scheme, f.session.design, fe::CoreAt (f.scheme, f.scheme.cores[1].centre)));
    EXPECT_EQ (f.scheme.cores.size (), 2u);
    // 960 m2 needs two stairs: taking another away leaves two.
    f.Apply (fe::RemoveStair (f.scheme, f.session.design, 0));
    EXPECT_EQ (f.scheme.cores.size (), 2u);
}

TEST (FloorSchemeEdit, AWallMovesAndItsNeighboursStay)
{
    Floor f ({ Rect (0, 0, 36, 16) });
    std::optional<fe::Wall> wall;
    for (size_t i = 0; i < f.scheme.flats.size () && !wall; ++i) {
        const auto& r = f.scheme.flats[i].shape;
        for (size_t k = 0; k < r.size () && !wall; ++k) {
            const fs::Vec a = r[k], b = r[(k + 1) % r.size ()];
            wall = fe::WallAt (f.scheme, { (a.x + b.x) / 2, (a.y + b.y) / 2 }, 0.3);
        }
    }
    ASSERT_TRUE (wall.has_value ());
    const double x = (wall->a.x + wall->b.x) / 2 + 0.8, y = (wall->a.y + wall->b.y) / 2;
    const size_t flats = f.scheme.flats.size ();
    f.Apply (fe::MoveWall (f.scheme, f.session.design, *wall, { x, y }));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    EXPECT_EQ (f.scheme.flats.size (), flats);
    bool moved = false;
    for (const auto& flat : f.scheme.flats)
        for (const auto& p : flat.shape)
            moved = moved || (std::abs (p.x - x) < 0.35 && std::abs (p.y - y) < 8.5);
    EXPECT_TRUE (moved) << "no flat edge where the wall was dragged";
}

TEST (FloorSchemeEdit, FlatsSplitJoinAndChangeRooms)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    // A band flat joins its neighbour, and the joined flat splits in two again.
    const size_t flats = f.scheme.flats.size ();
    int any = -1;
    for (size_t i = 0; i < f.scheme.flats.size () && any < 0; ++i)
        if (f.scheme.flats[i].band >= 0)
            any = static_cast<int> (i);
    ASSERT_GE (any, 0);
    const fs::Vec at = Mid (f.scheme.flats[any].shape);
    f.Apply (fe::RemoveFlat (f.scheme, f.session.design, any));
    EXPECT_EQ (f.scheme.flats.size (), flats - 1) << Report (f.scheme);
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    const int joined = fe::FlatAt (f.scheme, at);
    ASSERT_GE (joined, 0);
    f.Apply (fe::SplitFlat (f.scheme, f.session.design, joined, Mid (f.scheme.flats[joined].shape)));
    EXPECT_EQ (f.scheme.flats.size (), flats) << Report (f.scheme);
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    // A wide flat takes the room count asked for.
    int wide = -1;
    for (size_t i = 0; i < f.scheme.flats.size (); ++i)
        if (f.scheme.flats[i].band >= 0 && f.scheme.flats[i].frontage >= fs::RoomFrontage (3).min &&
            f.scheme.flats[i].rooms != 2)
            wide = static_cast<int> (i);
    ASSERT_GE (wide, 0);
    const fs::Vec mid = Mid (f.scheme.flats[wide].shape);
    f.Apply (fe::SetRooms (f.scheme, f.session.design, wide, 2));
    const int now = fe::FlatAt (f.scheme, mid);
    ASSERT_GE (now, 0);
    EXPECT_DOUBLE_EQ (f.scheme.flats[now].rooms, 2.0);
}

TEST (FloorSchemeEdit, UndoAndRedoSwapDesigns)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    const auto first = f.scheme.cores.size ();
    f.Apply (fe::AddStair (f.scheme, f.session.design, { 30, 12 }));
    ASSERT_NE (f.scheme.cores.size (), first);
    ASSERT_TRUE (f.session.Undo ());
    EXPECT_EQ (fe::Run (f.session.floor, f.session.programme, f.session.design).cores.size (), first);
    ASSERT_TRUE (f.session.Redo ());
    EXPECT_EQ (fe::Run (f.session.floor, f.session.programme, f.session.design).cores.size (), first + 1);
    f.session.Reset ();
    EXPECT_TRUE (f.session.design == fe::Design {});
    EXPECT_TRUE (f.session.Undo ());
}

TEST (FloorSchemeEdit, CorridorEndAndAccessFollowThePins)
{
    Floor f ({ Rect (0, 0, 36, 16) });
    const auto end = fe::EndAt (f.scheme, f.scheme.corridors.front ().axis.front (), 0.5);
    ASSERT_TRUE (end.has_value ());
    const fs::Vec to { end->at.x + (end->at.x < 18 ? 1.5 : -1.5), end->at.y };
    f.Apply (fe::MoveEnd (f.session.design, *end, to));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    double near = 1e9;
    for (const auto& c : f.scheme.corridors)
        for (const auto& p : { c.axis.front (), c.axis.back () })
            near = (std::min) (near, std::abs (p.x - to.x));
    EXPECT_LT (near, 0.3);

    Floor g ({ Rect (0, 0, 48, 12) });
    g.Apply (fe::SetAccess (g.scheme, g.session.design, fe::WingAt (g.scheme, { 24, 6 }), fs::Access::Rows));
    EXPECT_EQ (Errors (g.scheme), 0u) << Report (g.scheme);
    for (const auto& flat : g.scheme.flats)
        EXPECT_FALSE (flat.through);
}

namespace {
double AreaOf (const std::vector<fs::Ring>& rings)
{
    double a = 0;
    for (const auto& r : rings)
        a += fs::Area (r);
    return a;
}
// A party wall between two band flats, found by walking flat edges.
std::optional<fe::Wall> AnyWall (const fs::Scheme& s)
{
    for (const auto& flat : s.flats) {
        const auto& r = flat.shape;
        for (size_t k = 0; k < r.size (); ++k) {
            const fs::Vec a = r[k], b = r[(k + 1) % r.size ()];
            if (auto wall = fe::WallAt (s, { (a.x + b.x) / 2, (a.y + b.y) / 2 }, 0.3))
                return wall;
        }
    }
    return std::nullopt;
}
} // namespace

TEST (FloorSchemeEdit, FlatsChangePlacesInTheirBandAndAcrossTheCorridor)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    // Two flats of one band with different widths: each takes the other's place and width.
    int a = -1, b = -1;
    for (size_t i = 0; i < f.scheme.flats.size () && a < 0; ++i)
        for (size_t j = i + 1; j < f.scheme.flats.size (); ++j)
            if (f.scheme.flats[i].band >= 0 && f.scheme.flats[i].band == f.scheme.flats[j].band &&
                std::abs (f.scheme.flats[i].frontage - f.scheme.flats[j].frontage) > 0.5) {
                a = int (i), b = int (j);
                break;
            }
    ASSERT_GE (a, 0);
    const double wa = f.scheme.flats[a].frontage, wb = f.scheme.flats[b].frontage;
    const fs::Vec ma = Mid (f.scheme.flats[a].shape), mb = Mid (f.scheme.flats[b].shape);
    // The first flat's outer end along the band stays where the band begins.
    const fs::Vec firstEnd = ma.x < mb.x ? ma : mb;
    const double firstWidth = ma.x < mb.x ? wb : wa;
    f.Apply (fe::SwapFlats (f.scheme, f.session.design, a, b));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    double near = 1e9;
    for (const auto& flat : f.scheme.flats)
        if (flat.band >= 0 && std::abs (Mid (flat.shape).y - firstEnd.y) < 3)
            near = (std::min) (near, std::abs (flat.frontage - firstWidth));
    EXPECT_LT (near, 0.45) << "a flat of the other's width in the band";

    // Across the corridor: two flats facing each other take each other's room counts and widths.
    Floor g ({ Rect (0, 0, 60, 16) });
    int p = -1, q = -1;
    double best = 1e9;
    for (size_t i = 0; i < g.scheme.flats.size (); ++i)
        for (size_t j = 0; j < g.scheme.flats.size (); ++j) {
            const auto& fi = g.scheme.flats[i];
            const auto& fj = g.scheme.flats[j];
            const double dx = std::abs (Mid (fi.shape).x - Mid (fj.shape).x);
            if (fi.band >= 0 && fj.band >= 0 && Mid (fi.shape).y < 8 && Mid (fj.shape).y > 8 &&
                std::abs (fi.frontage - fj.frontage) > 0.8 && dx < best)
                best = dx, p = int (i), q = int (j);
        }
    ASSERT_GE (p, 0);
    const double rp = g.scheme.flats[p].rooms, rq = g.scheme.flats[q].rooms;
    const double wp = g.scheme.flats[p].frontage, wq = g.scheme.flats[q].frontage;
    const fs::Vec mp = Mid (g.scheme.flats[p].shape), mq = Mid (g.scheme.flats[q].shape);
    g.Apply (fe::SwapFlats (g.scheme, g.session.design, p, q));
    EXPECT_EQ (Errors (g.scheme), 0u) << Report (g.scheme);
    const int np = fe::FlatAt (g.scheme, mp), nq = fe::FlatAt (g.scheme, mq);
    ASSERT_GE (np, 0) << Report (g.scheme);
    ASSERT_GE (nq, 0) << Report (g.scheme);
    EXPECT_DOUBLE_EQ (g.scheme.flats[np].rooms, rq);
    EXPECT_DOUBLE_EQ (g.scheme.flats[nq].rooms, rp);
    // Widths follow where a neighbour in the band can give or take the difference.
    for (const auto& [now, width] : { std::pair { np, wq }, std::pair { nq, wp } }) {
        int shared = 0;
        for (const auto& flat : g.scheme.flats)
            shared += flat.band == g.scheme.flats[now].band;
        if (shared > 1)
            EXPECT_NEAR (g.scheme.flats[now].frontage, width, 0.45);
    }
}

TEST (FloorSchemeEdit, AFlatPutInSplitsTheFlatThereAndTheCountFollowsTheScroller)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    const size_t flats = f.scheme.flats.size ();
    // A flat that can take another beside it, both reaching the corridor.
    int wide = -1;
    for (size_t i = 0; i < f.scheme.flats.size () && wide < 0; ++i)
        if (fe::Insert (f.scheme, Mid (f.scheme.flats[i].shape), 2))
            wide = int (i);
    ASSERT_GE (wide, 0);
    const fs::Vec at = Mid (f.scheme.flats[wide].shape);
    const auto line = fe::Insert (f.scheme, at, 2);
    ASSERT_TRUE (line.has_value ());
    EXPECT_NEAR (std::hypot (line->b.x - line->a.x, line->b.y - line->a.y),
                 f.scheme.bands[f.scheme.flats[wide].band].depth, 1e-6)
        << "the divider spans the band";
    f.Apply (fe::InsertFlat (f.scheme, f.session.design, at, 2));
    EXPECT_EQ (f.scheme.flats.size (), flats + 1) << Report (f.scheme);
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    const int made = fe::FlatAt (f.scheme, Mid (line->region));
    ASSERT_GE (made, 0);
    // The room count asked for wherever its width allows it.
    if (f.scheme.flats[made].frontage >= fs::RoomFrontage (2).min)
        EXPECT_DOUBLE_EQ (f.scheme.flats[made].rooms, 2.0);

    Floor g ({ Rect (0, 0, 60, 16) });
    g.Apply (fe::SetCount (g.scheme, g.session.design, int (flats) + 1));
    EXPECT_EQ (g.scheme.flats.size (), flats + 1) << Report (g.scheme);
    g.Apply (fe::SetCount (g.scheme, g.session.design, int (flats) - 2));
    EXPECT_EQ (g.scheme.flats.size (), flats - 2) << Report (g.scheme);
    EXPECT_EQ (Errors (g.scheme), 0u) << Report (g.scheme);
}

TEST (FloorSchemeEdit, ALockedFlatKeepsItsPlaceWhenTheCountDrops)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    int pick = -1;
    for (size_t i = 0; i < f.scheme.flats.size () && pick < 0; ++i)
        if (f.scheme.flats[i].band >= 0 && !f.scheme.flats[i].corner)
            pick = int (i);
    ASSERT_GE (pick, 0);
    const auto shape = f.scheme.flats[pick].shape;
    f.Apply (fe::SetLocked (f.scheme, f.session.design, pick, true));
    const int locked = fe::FlatAt (f.scheme, Mid (shape));
    ASSERT_TRUE (fe::Locked (f.scheme, f.session.design, locked));
    EXPECT_TRUE (fe::RemoveFlat (f.scheme, f.session.design, locked) == f.session.design) << "a locked flat stays";
    f.Apply (fe::SetCount (f.scheme, f.session.design, int (f.scheme.flats.size ()) - 3));
    const int still = fe::FlatAt (f.scheme, Mid (shape));
    ASSERT_GE (still, 0);
    EXPECT_NEAR (fs::Area (f.scheme.flats[still].shape), fs::Area (shape), 1.0);
    f.Apply (fe::SetLocked (f.scheme, f.session.design, still, false));
    EXPECT_TRUE (f.session.design.locked.empty ());
}

TEST (FloorSchemeEdit, APartyWallPushedTakesTheNeighboursFloorAndGivesItBack)
{
    // Two buildings side by side, one 36 m and one 24 m long, both 16 deep.
    const std::vector<std::vector<fs::Ring>> floors { { Rect (0, 0, 36, 16) }, { Rect (36, 0, 60, 16) } };
    fe::Design mine, theirs;
    auto owned = fe::Owned (floors, { &mine, &theirs });
    EXPECT_NEAR (AreaOf (owned[0]), 576, 1e-6);
    EXPECT_NEAR (AreaOf (owned[1]), 384, 1e-6);
    const auto scheme = fe::RunOwned (owned[0], owned[1], fp::Default (), mine);
    ASSERT_EQ (scheme.party.size (), 1u);
    // Pushed 4 m into the neighbour.
    fe::Take (mine, theirs, fe::PartyStrip (scheme.party[0], 4, owned[1]));
    owned = fe::Owned (floors, { &mine, &theirs });
    EXPECT_NEAR (AreaOf (owned[0]), 576 + 64, 1e-6);
    EXPECT_NEAR (AreaOf (owned[1]), 384 - 64, 1e-6);
    // The neighbour pushes back 6 m: the 4 it lost and 2 more.
    const auto there = fe::RunOwned (owned[1], owned[0], fp::Default (), theirs);
    ASSERT_EQ (there.party.size (), 1u);
    fe::Take (theirs, mine, fe::PartyStrip (there.party[0], 6, owned[0]));
    owned = fe::Owned (floors, { &mine, &theirs });
    EXPECT_NEAR (AreaOf (owned[0]), 576 - 32, 1e-6);
    EXPECT_NEAR (AreaOf (owned[1]), 384 + 32, 1e-6);
    EXPECT_NEAR (AreaOf (owned[0]) + AreaOf (owned[1]), 960, 1e-6) << "nothing lost or doubled";
    // Overlapping floors: the earlier building keeps the overlap.
    const auto overlap = fe::Owned ({ { Rect (0, 0, 40, 16) }, { Rect (36, 0, 60, 16) } }, { nullptr, nullptr });
    EXPECT_NEAR (AreaOf (overlap[0]) + AreaOf (overlap[1]), 960, 1e-6);
}

TEST (FloorSchemeEdit, DraggedWallsAndStairsMoveInStepsAndSnapToProgrammeSizes)
{
    Floor f ({ Rect (0, 0, 36, 16) });
    const auto wall = AnyWall (f.scheme);
    ASSERT_TRUE (wall.has_value ());
    const fs::Vec from = wall->a;
    const auto programme = fp::Default ();
    for (double t : { 0.13, 0.55, 1.07, -0.91 }) {
        const fs::Vec to { from.x + wall->axis.x * t, from.y + wall->axis.y * t };
        const fs::Vec got = fe::SnapWall (f.scheme, *wall, from, to, programme);
        const double moved = (got.x - from.x) * wall->axis.x + (got.y - from.y) * wall->axis.y;
        EXPECT_LE (std::abs (moved - t), fe::kStep / 2 + 1e-9) << t;
        // On the 0.4 m step, or where one of the two flats has a type's middle area.
        bool sized = std::abs (moved / fe::kStep - std::round (moved / fe::kStep)) < 1e-9;
        for (const int flat : { wall->low, wall->high })
            for (const auto& type : programme.types) {
                const auto& fl = f.scheme.flats[flat];
                const double frontage = fl.frontage + (flat == wall->low ? moved : -moved);
                sized = sized || std::abs (fs::NetArea (frontage, fl.depth) - (type.minM2 + type.maxM2) / 2) < 1e-6;
            }
        EXPECT_TRUE (sized) << t;
    }
    const fs::Vec c = f.scheme.cores[0].centre;
    const fs::Vec s = fe::Stepped (f.scheme, c, { c.x + 1.13, c.y - 0.31 });
    EXPECT_NEAR (std::abs (s.x - c.x) + std::abs (s.y - c.y), 1.6, 1e-9) << "1.2 along and 0.4 across";
}
