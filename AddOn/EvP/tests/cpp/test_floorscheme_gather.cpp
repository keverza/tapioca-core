// What the user's stairs and corridor ends leave beside them (user, 2026-11-15): a stack stair
// stands anywhere along its corridor and the flats beside it are resized; a corridor's end runs on
// past its stair and the unit round it is an L; a strip left to nobody joins the unit beside it;
// and a thin bar keeps its corridor on one edge rather than two halls round one stair.
#include "ArchViz/FloorSchemeEdit.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace fs = geomsrv::archviz::floorscheme;
namespace fp = geomsrv::archviz::floorprogramme;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
double Smallest ()
{
    double least = 1e18;
    for (const auto& type : fp::Default ().types)
        least = (std::min) (least, type.minM2);
    return least;
}
std::string Report (const fs::Scheme& s)
{
    std::ostringstream out;
    out << s.flats.size () << " flats, " << s.cores.size () << " stairs\n";
    for (const auto& u : s.unassigned)
        out << "  unassigned: " << u.reason << "\n";
    for (const auto& d : s.diagnostics)
        if (d.level != fs::Diagnostic::Info)
            out << "  " << d.code << " " << d.text << "\n";
    return out.str ();
}
size_t Red (const fs::Scheme& s)
{
    return std::count_if (s.unassigned.begin (), s.unassigned.end (),
                          [] (const fs::Piece& p) { return p.reason.find ("storage") == std::string::npos; });
}
// A 60 x 16 m bar as a building has it: its two stairs the stack's, held where they stand.
struct Held {
    std::vector<fs::Ring> outline { Rect (0, 0, 60, 16) };
    fs::Scheme plain = fs::Generate (outline, fp::Default ());
    fs::Options options;
    fs::Pins pins;
    Held ()
    {
        options.holdCores = true;
        for (const auto& core : plain.cores)
            pins.cores.push_back ({ core.centre, core.width, core.depth });
    }
};
} // namespace

TEST (FloorSchemeGather, AStackStairStandsAnywhereAlongItsCorridorAndTheFlatsAreResized)
{
    Held h;
    ASSERT_EQ (h.plain.cores.size (), 2u);
    const double first = h.pins.cores[0].centre.x;
    for (const double x : { first, 12.0, 15.2, 18.0, 22.4, 27.0 }) {
        h.pins.cores[0].centre.x = x;
        const auto s = fs::Generate (h.outline, fp::Default (), h.pins, h.options);
        EXPECT_TRUE (s.ok) << "stair at " << x << "\n" << Report (s);
        EXPECT_EQ (Red (s), 0u) << "stair at " << x << "\n" << Report (s);
        EXPECT_TRUE (std::any_of (s.cores.begin (), s.cores.end (),
                                  [&] (const fs::Core& c) { return std::abs (c.centre.x - x) < 1e-6; }))
            << "the stair stands where it was put: " << x;
        for (const auto& f : s.flats)
            EXPECT_GE (f.net, Smallest () - 0.5) << "stair at " << x << ": no flat under the smallest";
        EXPECT_NEAR (s.net, h.plain.net, 0.12 * h.plain.net) << "stair at " << x << ": the floor is still flats";
    }
}

TEST (FloorSchemeGather, ACorridorRunsOnPastItsStairAndTheUnitRoundItIsAnL)
{
    Held h;
    // The first corridor ends at its stair; its end is pulled on to 3 m from the gable.
    const fs::Vec end = h.plain.corridors[0].axis.front ();
    ASSERT_GT (end.x, 6.0);
    h.pins.ends.push_back ({ { 3.0, end.y } });
    const auto s = fs::Generate (h.outline, fp::Default (), h.pins, h.options);
    EXPECT_TRUE (s.ok) << Report (s);
    EXPECT_EQ (Red (s), 0u) << Report (s);
    ASSERT_FALSE (s.corridors.empty ());
    EXPECT_NEAR (s.corridors[0].axis.front ().x, 3.5, 1e-6) << "as near the gable as an end flat allows";
    EXPECT_NEAR (s.cores[0].centre.x, h.pins.cores[0].centre.x, 1e-6) << "the stair has not moved";
    // The unit round the corridor's end: between the gable and the stair, not a rectangle.
    const auto unit = std::find_if (s.flats.begin (), s.flats.end (), [&] (const fs::Flat& f) {
        double x0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (const auto& p : f.shape)
            x0 = (std::min) (x0, p.x), x1 = (std::max) (x1, p.x), y1 = (std::max) (y1, p.y);
        return x0 < 1e-6 && y1 > 15.9 && x1 > 3.6;
    });
    ASSERT_NE (unit, s.flats.end ()) << Report (s);
    EXPECT_GT (unit->shape.size (), 4u) << "an L";
    EXPECT_GE (unit->net, Smallest () - 0.5);
}

TEST (FloorSchemeGather, AThinBarKeepsItsCorridorOnOneEdgeRatherThanTwoHallsRoundOneStair)
{
    // 37.1 x 9.4 m with the building's one stair across it (the user's case).
    const std::vector<fs::Ring> outline { Rect (0, 0, 37.1, 9.4) };
    fs::Options o;
    o.holdCores = true;
    fs::Pins pins;
    pins.cores.push_back ({ { 18.5, 4.7 }, 4.5, 9.4 });
    const auto candidates = fs::Candidates (outline, fp::Default (), pins, o);
    const auto chosen =
        std::find_if (candidates.begin (), candidates.end (), [] (const fs::Candidate& c) { return c.chosen; });
    ASSERT_NE (chosen, candidates.end ());
    const auto s = fs::Generate (outline, fp::Default (), pins, o);
    ASSERT_EQ (s.wings.size (), 1u);
    EXPECT_EQ (s.wings[0].access, fs::Access::OneSide) << Report (s);
    EXPECT_GE (s.flats.size (), 4u) << Report (s);
    EXPECT_FALSE (s.corridors.empty ());
    for (const auto& f : s.flats)
        EXPECT_LE (f.net, fs::MaxFlat (fp::Default (), o) + 0.5) << "no hall";
}
