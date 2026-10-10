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
