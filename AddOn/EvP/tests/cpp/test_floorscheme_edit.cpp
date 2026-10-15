#include "ArchViz/FloorSchemeEdit.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <optional>
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

TEST (FloorSchemeEdit, StairsMoveAddAndGoDownToTheEscapeMinimum)
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
    // User, 2026-10-14: "I was not able to remove stairs even if would result in better plan." 960 m2
    // is over one stair's floor, but one stair reaches every flat of a 60 m bar: it goes, and the
    // scheme says the floor per stair is over.
    std::string why;
    f.Apply (fe::RemoveStair (f.scheme, f.session.design, 0, &why));
    EXPECT_TRUE (why.empty ()) << why;
    EXPECT_EQ (f.scheme.cores.size (), 1u) << Report (f.scheme);
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    EXPECT_TRUE (std::any_of (f.scheme.diagnostics.begin (), f.scheme.diagnostics.end (),
                              [] (const fs::Diagnostic& d) { return d.code == "core.area"; }));
    // The last stair of a wing stays, and says why.
    why.clear ();
    EXPECT_EQ (fe::RemoveStair (f.scheme, f.session.design, 0, &why), f.session.design);
    EXPECT_NE (why.find ("only stair"), std::string::npos) << why;

    // A 150 m bar: stairs go down to as many as keep every flat within reach of one, then stay.
    Floor g ({ Rect (0, 0, 150, 16) });
    size_t left = g.scheme.cores.size ();
    for (int n = 0; n < 8 && why.find ("this one stays") == std::string::npos; ++n) {
        why.clear ();
        const auto next = fe::RemoveStair (g.scheme, g.session.design, 0, &why);
        if (next == g.session.design)
            break;
        g.Apply (next);
        EXPECT_EQ (g.scheme.cores.size (), left - 1) << Report (g.scheme);
        left = g.scheme.cores.size ();
    }
    EXPECT_NE (why.find ("from a stair"), std::string::npos) << why;
    EXPECT_GE (g.scheme.cores.size (), 3u) << "150 m of bar needs three stairs within reach";
    EXPECT_EQ (Errors (g.scheme), 0u) << Report (g.scheme);
}

TEST (FloorSchemeEdit, AWallMovesAndItsNeighboursStay)
{
    Floor f ({ Rect (0, 0, 36, 16) });
    std::optional<fe::Wall> wall;
    for (size_t i = 0; i < f.scheme.flats.size () && !wall; ++i) {
        const auto& r = f.scheme.flats[i].shape;
        for (size_t k = 0; k < r.size () && !wall; ++k) {
            const fs::Vec a = r[k], b = r[(k + 1) % r.size ()];
            if (auto w = fe::WallAt (f.scheme, { (a.x + b.x) / 2, (a.y + b.y) / 2 }, 0.3); w && !w->seam)
                wall = w;
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

// User, 2026-10-14: "There needs to be text line to say if user interaction is invalid, silent
// failure is bad UI/UX." An edit that cannot be made leaves the design as it was and says why.
TEST (FloorSchemeEdit, AnEditThatCannotBeMadeSaysWhy)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    ASSERT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    int cap = -1, row = -1;
    for (size_t i = 0; i < f.scheme.flats.size (); ++i)
        (f.scheme.flats[i].band < 0 ? cap : row) = static_cast<int> (i);
    ASSERT_GE (cap, 0) << "the bar has corridor-end flats";
    ASSERT_GE (row, 0);
    std::string why;
    EXPECT_EQ (fe::RemoveFlat (f.scheme, f.session.design, int (f.scheme.flats.size ()), &why), f.session.design);
    EXPECT_NE (why.find ("no flat there"), std::string::npos) << why;
    // A locked flat neither splits nor goes.
    f.Apply (fe::SetLocked (f.scheme, f.session.design, row, true));
    const int locked = fe::FlatAt (f.scheme, Mid (f.scheme.flats[row].shape));
    ASSERT_GE (locked, 0);
    why.clear ();
    EXPECT_EQ (fe::SplitFlat (f.scheme, f.session.design, locked, Mid (f.scheme.flats[locked].shape), &why),
               f.session.design);
    EXPECT_NE (why.find ("locked"), std::string::npos) << why;
    // A count past what the floor takes is made as far as it goes, and says where it stopped.
    why.clear ();
    const auto many = fe::SetCount (f.scheme, f.session.design, 500, &why);
    EXPECT_FALSE (many == f.session.design) << "the floor still took more flats";
    EXPECT_NE (why.find ("as many as the floor takes"), std::string::npos) << why;
    // Nothing under the drop: a flat put in says where it must go.
    why.clear ();
    EXPECT_FALSE (fe::Insert (f.scheme, { -50, -50 }, 2, &why).has_value ());
    EXPECT_FALSE (why.empty ());
    // An edit that can be made says nothing.
    why.clear ();
    const int other = fe::FlatAt (f.scheme, Mid (f.scheme.flats[row == 0 ? 1 : 0].shape));
    fe::SplitFlat (f.scheme, f.session.design, other, Mid (f.scheme.flats[other].shape), &why);
    if (f.scheme.flats[other].band >= 0)
        EXPECT_TRUE (why.empty () || why.find ("narrow") != std::string::npos) << why;
}

// User, 2026-10-14: "Sometimes after drag and drop room becomes large like 5R but color and name is
// like 1.5R." Across the corridor a flat takes the other's room count with its width; where its row
// cannot give or take the difference it keeps its own width, and only if that suits the count.
TEST (FloorSchemeEdit, ARoomCountNeverMovesWithoutItsWidth)
{
    // A small flat across the corridor from a wide flat alone in its row: the wide one cannot narrow
    // and cannot hold the small count, so the swap is refused rather than mislabelled.
    Floor h ({ Rect (0, 0, 60, 16) });
    const auto alone = [&] (int flat) {
        int shared = 0;
        for (const auto& f : h.scheme.flats)
            shared += f.band == h.scheme.flats[flat].band;
        return shared == 1;
    };
    int p = -1, q = -1;
    for (size_t i = 0; i < h.scheme.flats.size () && p < 0; ++i)
        for (size_t j = 0; j < h.scheme.flats.size (); ++j) {
            const auto& fi = h.scheme.flats[i];
            const auto& fj = h.scheme.flats[j];
            if (fi.band >= 0 && fj.band >= 0 && Mid (fi.shape).y < 8 && Mid (fj.shape).y > 8 && alone (int (j)) &&
                fj.frontage > fs::RoomFrontage (fi.rooms).max + 0.5) {
                p = int (i), q = int (j);
                break;
            }
        }
    ASSERT_GE (p, 0) << Report (h.scheme);
    std::string why;
    EXPECT_EQ (fe::SwapFlats (h.scheme, h.session.design, p, q, &why), h.session.design);
    EXPECT_NE (why.find ("cannot hold"), std::string::npos) << why;

    // A swap that is made leaves both flats' widths suiting their new counts.
    int made = 0;
    for (size_t i = 0; i < h.scheme.flats.size (); ++i)
        for (size_t j = 0; j < h.scheme.flats.size (); ++j) {
            const auto& fi = h.scheme.flats[i];
            const auto& fj = h.scheme.flats[j];
            if (fi.band < 0 || fj.band < 0 || Mid (fi.shape).y > 8 || Mid (fj.shape).y < 8 || fi.rooms == fj.rooms)
                continue;
            const auto next = fe::SwapFlats (h.scheme, h.session.design, int (i), int (j));
            if (next == h.session.design)
                continue;
            ++made;
            Floor k ({ Rect (0, 0, 60, 16) });
            k.Apply (next);
            for (const auto& at : { Mid (fi.shape), Mid (fj.shape) }) {
                const int now = fe::FlatAt (k.scheme, at);
                ASSERT_GE (now, 0) << Report (k.scheme);
                const auto& f = k.scheme.flats[now];
                const auto suits = fs::RoomFrontage (f.rooms);
                EXPECT_GE (f.frontage, suits.min - 0.6) << f.rooms << "R, " << Report (k.scheme);
                EXPECT_LE (f.frontage, suits.max + 0.6) << f.rooms << "R, " << Report (k.scheme);
            }
        }
    EXPECT_GT (made, 0) << "some swap across the corridor is still made";
}

// User, 2026-10-14: "I was not able to delete some units." A flat at a corridor's end, and the only
// flat of its row, have no row to close over them: each joins the flat it shares the longest wall
// with, one flat of both areas whose rooms are laid again.
TEST (FloorSchemeEdit, AFlatWithoutARowJoinsTheFlatBesideIt)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    ASSERT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    const double most = fs::MaxFlat (f.session.programme, f.session.options);
    std::vector<int> rowless;
    for (size_t i = 0; i < f.scheme.flats.size (); ++i) {
        int shared = 0;
        for (const auto& other : f.scheme.flats)
            shared += f.scheme.flats[i].band >= 0 && other.band == f.scheme.flats[i].band;
        if (f.scheme.flats[i].band < 0 || shared == 1)
            rowless.push_back (static_cast<int> (i));
    }
    ASSERT_FALSE (rowless.empty ()) << "the bar has corridor-end flats";
    int removed = 0;
    for (const int i : rowless) {
        Floor g ({ Rect (0, 0, 60, 16) });
        const auto& gone = g.scheme.flats[i];
        const fs::Vec at = Mid (gone.shape);
        const double net = g.scheme.net;
        std::string why;
        const auto next = fe::RemoveFlat (g.scheme, g.session.design, i, &why, most);
        if (next == g.session.design) {
            EXPECT_FALSE (why.empty ()) << "a flat that stays says why";
            continue;
        }
        ++removed;
        const size_t flats = g.scheme.flats.size ();
        g.Apply (next);
        EXPECT_EQ (g.scheme.flats.size (), flats - 1) << Report (g.scheme);
        EXPECT_EQ (Errors (g.scheme), 0u) << Report (g.scheme);
        EXPECT_NEAR (g.scheme.net, net, 1.0) << "the floor it had goes to the flat beside it";
        const int joined = fe::FlatAt (g.scheme, at);
        ASSERT_GE (joined, 0) << "its floor is a flat's";
        const auto& flat = g.scheme.flats[joined];
        // Over the largest flat it is still made, as asked, and says so.
        EXPECT_EQ (flat.net > most + 0.5, why.find ("over the largest flat") != std::string::npos)
            << flat.net << " m2: " << why;
        EXPECT_FALSE (flat.roomList.empty () && !flat.manual) << "its rooms are laid, or left to the user";
        // Its type follows its area: no large flat named for a small count.
        const auto& type = g.session.programme.types[flat.type];
        EXPECT_EQ (flat.rooms, type.rooms);
        EXPECT_GE (flat.net, type.minM2 - 0.5) << flat.net << " m2 as " << flat.rooms << "R";
        // The design keeps the join: it reads back, and is undone like any edit.
        ASSERT_TRUE (g.session.Undo ());
        g.scheme = fe::Run (g.session.floor, g.session.programme, g.session.design);
        EXPECT_EQ (g.scheme.flats.size (), flats);
    }
    EXPECT_GT (removed, 0) << "a corridor-end flat can be removed";
}

// User, 2026-10-14: "I was not able to increase unit count to make them all 1R. Decrease count to
// minimum." The most flats is every row flat the smallest type the programme has; the fewest joins
// every row and then the flats with no row to close over them.
TEST (FloorSchemeEdit, TheMostFlatsAreAllTheSmallestAndTheFewestAreFewerThanRowsAllow)
{
    const auto programme = fp::Default ();
    double fewest = 1e9;
    for (const auto& type : programme.types)
        fewest = (std::min) (fewest, type.rooms);
    EXPECT_DOUBLE_EQ (fe::Narrowest (programme), fs::RoomFrontage (fewest).min);

    Floor f ({ Rect (0, 0, 60, 16) });
    const size_t flats = f.scheme.flats.size ();
    std::string why;
    f.Apply (fe::SetCount (f.scheme, f.session.design, 500, &why, fe::Narrowest (programme)));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    EXPECT_NE (why.find ("as many as the floor takes"), std::string::npos) << why;
    EXPECT_GT (f.scheme.flats.size (), flats + 2) << Report (f.scheme);
    // Every flat of a row between its ends is the smallest type; the ends may be wider, reaching
    // the corridor by a door.
    for (size_t b = 0; b < f.scheme.bands.size (); ++b) {
        std::vector<const fs::Flat*> row;
        for (const auto& flat : f.scheme.flats)
            if (flat.band == static_cast<int> (b))
                row.push_back (&flat);
        std::sort (row.begin (), row.end (), [&] (const fs::Flat* x, const fs::Flat* y) {
            const fs::Vec a = Mid (x->shape), c = Mid (y->shape);
            return a.x + a.y < c.x + c.y;
        });
        for (size_t k = 1; k + 1 < row.size (); ++k)
            EXPECT_DOUBLE_EQ (row[k]->rooms, fewest) << row[k]->frontage << " m wide in row " << b;
    }
    // And no row takes one more.
    const auto again = fe::SetCount (f.scheme, f.session.design, 500, nullptr, fe::Narrowest (programme));
    EXPECT_TRUE (again == f.session.design) << "Max twice adds a flat";

    // The fewest from there: back to flats no larger than the largest the programme has.
    const double most = fs::MaxFlat (programme, f.session.options);
    const size_t many = f.scheme.flats.size ();
    why.clear ();
    f.Apply (fe::SetCount (f.scheme, f.session.design, 1, &why, fe::Narrowest (programme), most));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    EXPECT_NE (why.find ("over the largest flat"), std::string::npos) << why;
    EXPECT_LE (f.scheme.flats.size (), flats) << Report (f.scheme);
    EXPECT_LT (f.scheme.flats.size (), many);
    for (const auto& flat : f.scheme.flats)
        if (flat.band >= 0)
            EXPECT_LE (flat.net, most + 8) << flat.frontage << " m wide";

    // With no limit, past one flat a row: the flats with no row join the flats beside them.
    Floor g ({ Rect (0, 0, 60, 16) });
    int rows = 0;
    for (const auto& band : g.scheme.bands)
        rows += band.flats > 0;
    g.Apply (fe::SetCount (g.scheme, g.session.design, 1, nullptr, fe::Narrowest (programme)));
    EXPECT_LT (static_cast<int> (g.scheme.flats.size ()), rows) << "past one flat a row: " << Report (g.scheme);
    const size_t joins = g.session.design.pins.joins.size ();
    EXPECT_GT (joins, 0u);
    // One more undoes the latest join.
    const size_t fewestFlats = g.scheme.flats.size ();
    g.Apply (fe::SetCount (g.scheme, g.session.design, int (fewestFlats) + 1, nullptr, fe::Narrowest (programme)));
    EXPECT_EQ (g.session.design.pins.joins.size (), joins - 1);
    EXPECT_EQ (g.scheme.flats.size (), fewestFlats + 1) << Report (g.scheme);
}

// User, 2026-10-14: "I was not able to move stair to other corridor side." A stair dropped across
// the corridor stands there, the section's caps with it.
TEST (FloorSchemeEdit, AStairMovesToTheOtherSideOfTheCorridor)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    ASSERT_EQ (f.scheme.cores.size (), 2u);
    const fs::Vec from = f.scheme.cores[0].centre, to { from.x, 16 - from.y };
    ASSERT_GT (std::abs (from.y - 8), 1.0) << "the stair stands on one side of the corridor";
    f.Apply (fe::MoveStair (f.scheme, f.session.design, 0, to));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    ASSERT_EQ (f.scheme.cores.size (), 2u);
    int across = 0, stayed = 0;
    for (const auto& c : f.scheme.cores) {
        across += std::abs (c.centre.x - from.x) < 3 && (c.centre.y - 8) * (from.y - 8) < 0;
        stayed += std::abs (c.centre.x - from.x) >= 3 && (c.centre.y - 8) * (from.y - 8) > 0;
    }
    EXPECT_EQ (across, 1) << "the moved stair is across the corridor";
    EXPECT_EQ (stayed, 1) << "the other stays where it was";
}

// User, 2026-10-14: "Add ability to rotate stair core though right click menu." A stair's size is
// its own: the core takes its width along the corridor, a quarter turn swaps width and depth, and
// moving or re-spacing the stairs keeps it.
TEST (FloorSchemeEdit, AStairTurnsAndKeepsItsSize)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    ASSERT_EQ (f.scheme.cores.size (), 2u);
    const auto core = [&] (fs::Vec near) { return fe::CoreAt (f.scheme, near) >= 0 ? fe::CoreAt (f.scheme, near) : 0; };
    const fs::Vec at = f.scheme.cores[0].centre;
    const double before = f.scheme.cores[0].width;
    EXPECT_NEAR (before, fs::Options {}.coreWidth, 1e-6);

    // A quarter turn: 4.5 x 4.2 becomes 4.2 x 4.5, the core 4.2 along the corridor.
    f.Apply (fe::RotateStair (f.scheme, f.session.design, 0));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    EXPECT_NEAR (f.scheme.cores[core (at)].width, fs::Options {}.coreDepth, 0.05) << Report (f.scheme);

    // A straight stair: 9 m along the corridor.
    f.Apply (fe::ResizeStair (f.scheme, f.session.design, core (at), 9.0, 2.5));
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    const int straight = core (at);
    EXPECT_NEAR (f.scheme.cores[straight].width, 9.0, 0.05) << Report (f.scheme);

    // Moved along the corridor, it keeps its size.
    const fs::Vec moved { f.scheme.cores[straight].centre.x + 1.0, f.scheme.cores[straight].centre.y };
    f.Apply (fe::MoveStair (f.scheme, f.session.design, straight, moved));
    EXPECT_NEAR (f.scheme.cores[core (moved)].width, 9.0, 0.05) << Report (f.scheme);

    // Turned, 9 m across does not fit a 7 m band: it says so.
    f.Apply (fe::RotateStair (f.scheme, f.session.design, core (moved)));
    EXPECT_TRUE (std::any_of (f.scheme.diagnostics.begin (), f.scheme.diagnostics.end (), [] (const fs::Diagnostic& d) {
        return d.code == "core.too_deep";
    })) << Report (f.scheme);
    std::string why;
    EXPECT_EQ (fe::ResizeStair (f.scheme, f.session.design, 0, 1.0, 4.0, &why), f.session.design);
    EXPECT_FALSE (why.empty ());
}

// User, 2026-10-14: "I was not able to move corner unit wall to make it L-shaped." The wall between a
// corridor's end flat and the row's end flat beside it moves: the strip it sweeps goes to the flat it
// moves away from, and the flat that gives it is an L.
TEST (FloorSchemeEdit, AWallBetweenTwoSlotsMovesAndLeavesAnL)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    std::optional<fe::Wall> seam;
    for (size_t i = 0; i < f.scheme.flats.size () && !seam; ++i) {
        const auto& cap = f.scheme.flats[i];
        if (cap.band >= 0)
            continue;
        for (size_t k = 0; k < cap.shape.size () && !seam; ++k) {
            const fs::Vec a = cap.shape[k], b = cap.shape[(k + 1) % cap.shape.size ()];
            const auto wall = fe::WallAt (f.scheme, { (a.x + b.x) / 2, (a.y + b.y) / 2 }, 0.3);
            if (wall && wall->seam && wall->low == int (i) && f.scheme.flats[wall->high].band >= 0)
                seam = wall;
        }
    }
    ASSERT_TRUE (seam.has_value ()) << "a corridor's end flat beside a row's flat";
    const auto& giver = f.scheme.flats[seam->high];
    const size_t corners = giver.shape.size ();
    const double net = f.scheme.net, gave = giver.net;
    fs::Vec far = giver.shape.front ();
    for (const auto& p : giver.shape) // the corner of the giving flat farthest from the seam
        if (std::hypot (p.x - seam->a.x, p.y - seam->a.y) > std::hypot (far.x - seam->a.x, far.y - seam->a.y))
            far = p;
    const fs::Vec inside { far.x + (seam->a.x - far.x) * 0.05, far.y + (seam->a.y - far.y) * 0.05 };
    std::string why;
    const fs::Vec to { seam->a.x + seam->axis.x * 2.0, seam->a.y + seam->axis.y * 2.0 };
    f.Apply (fe::MoveWall (f.scheme, f.session.design, *seam, to, &why));
    EXPECT_TRUE (why.empty ()) << why;
    EXPECT_EQ (Errors (f.scheme), 0u) << Report (f.scheme);
    EXPECT_NEAR (f.scheme.net, net, 1.0) << "the floor changes flats, none is lost";
    const int now = fe::FlatAt (f.scheme, inside);
    ASSERT_GE (now, 0);
    EXPECT_GT (f.scheme.flats[now].shape.size (), corners) << "an L";
    EXPECT_LT (f.scheme.flats[now].net, gave - 5.0);
    // Pushed right through the corridor's end flat, nothing of it would be left; nearly through the
    // row's flat, a sliver would be: it stays and says why.
    Floor g ({ Rect (0, 0, 60, 16) });
    for (const double t : { -20.0, 6.9 }) {
        why.clear ();
        const fs::Vec push { seam->a.x + seam->axis.x * t, seam->a.y + seam->axis.y * t };
        EXPECT_EQ (fe::MoveWall (g.scheme, g.session.design, *seam, push, &why), g.session.design) << t;
        EXPECT_FALSE (why.empty ()) << t;
    }
}

// User, 2026-10-14: "Show typology block division lines (the earlier stages of generator) to
// manually move or edit how big the stair core zone has to be. within such zone if more units are
// added you may extend corridor to reach them." A two-stair bar has one division between its stair
// zones; moved along the wing it stays where it was put, each zone keeping its stair, and the zone
// that grows takes the floor. A stair edit plans the zones again.
TEST (FloorSchemeEdit, AStairZonesDivisionMovesAndTheZonesFollow)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    ASSERT_EQ (f.scheme.cores.size (), 2u);
    ASSERT_EQ (f.scheme.divisions.size (), 1u) << "two stair zones, one division";
    const auto& line = f.scheme.divisions.front ();
    EXPECT_NEAR (line[0].x, line[1].x, 1e-6) << "across the wing";
    const double was = line[0].x;
    EXPECT_EQ (fe::DivisionAt (f.scheme, { was, 8 }, 0.3), 0);
    // Moved 3 m either way, as far as the stairs let it. Toward the first stair, the second zone's
    // pinned stair once had its cap search pick a 5 m corridor-end flat no flat fills, leaving the
    // corridor's end in floor nobody owned (corridor.open_end).
    double moved = was;
    for (const double by : { 3.0, -3.0 }) {
        Floor g ({ Rect (0, 0, 60, 16) });
        std::string why;
        const auto next = fe::MoveDivision (g.scheme, g.session.design, 0, { was + by, 8 }, &why);
        if (next == g.session.design)
            continue;
        g.Apply (next);
        EXPECT_EQ (Errors (g.scheme), 0u) << Report (g.scheme);
        ASSERT_EQ (g.scheme.divisions.size (), 1u);
        ASSERT_EQ (g.scheme.cores.size (), 2u);
        moved = g.scheme.divisions.front ()[0].x;
        EXPECT_GT ((moved - was) * by, 0.5) << "it moved the way it was dragged";
        // Each zone keeps its stair: one stair either side of the division.
        int below = 0;
        for (const auto& core : g.scheme.cores)
            below += core.centre.x < moved;
        EXPECT_EQ (below, 1);
        // A stair moved plans the zones again: the division leaves where it was put.
        g.Apply (fe::MoveStair (g.scheme, g.session.design, 0, g.scheme.cores[0].centre));
        EXPECT_TRUE (g.session.design.pins.cuts.empty ());
    }
    EXPECT_NE (moved, was) << "the division moved";
}

// A flat-count edit holds every row's flats, not only the row it laid again: planned again, the
// floor's mix must not take a flat back from an untouched row (with the floor planned to its main
// walls, one more flat in a row cost another row one -- the count unchanged).
TEST (FloorSchemeEdit, ACountChangedHoldsEveryRowsFlats)
{
    Floor f ({ Rect (0, 0, 60, 16) });
    const int flats = int (f.scheme.flats.size ());
    const auto d = fe::SetCount (f.scheme, f.session.design, flats + 1);
    for (size_t b = 0; b < f.scheme.bands.size (); ++b) {
        bool row = false, held = false;
        for (const auto& flat : f.scheme.flats)
            if (flat.band == int (b)) {
                row = true;
                held =
                    held || std::any_of (d.pins.counts.begin (), d.pins.counts.end (), [&] (const fs::Pins::Count& c) {
                        return fe::FlatAt (f.scheme, c.at) >= 0 &&
                               f.scheme.flats[fe::FlatAt (f.scheme, c.at)].band == int (b);
                    });
            }
        EXPECT_TRUE (!row || held) << "row " << b << " holds its count";
    }
    // Nothing asked, nothing pinned.
    const auto same = fe::SetCount (f.scheme, f.session.design, flats);
    EXPECT_TRUE (same.pins.counts.empty ());
}
