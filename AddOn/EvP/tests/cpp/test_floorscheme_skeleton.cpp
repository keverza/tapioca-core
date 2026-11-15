// The skeleton as the user's: wings taken instead of searched for (Pins::wings), a floor planned
// only as far as its bands (Analyse), a corridor moved off the middle (Pins::Corridor), and the
// edits that make them. The contract every test here holds to: the wings a scheme was planned
// with plan that scheme again, and a wing the floor cannot take is said and left out, never bent.
#include "ArchViz/FloorSchemeDesigns.hpp"
#include "ArchViz/FloorSchemeEdit.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace fs = geomsrv::archviz::floorscheme;
namespace fe = geomsrv::archviz::floorscheme::edit;
namespace fp = geomsrv::archviz::floorprogramme;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
fs::Ring Turned (fs::Ring ring, double degrees, fs::Vec origin = {})
{
    const double a = degrees * std::numbers::pi / 180, c = std::cos (a), s = std::sin (a);
    for (auto& p : ring)
        p = { origin.x + p.x * c - p.y * s, origin.y + p.x * s + p.y * c };
    return ring;
}
fs::Ring Reversed (fs::Ring ring)
{
    std::reverse (ring.begin (), ring.end ());
    return ring;
}
struct Case {
    const char* name;
    std::vector<fs::Ring> outline;
};
// The generator's own standard shapes (test_floorscheme.cpp), every typology it names.
std::vector<Case> Cases ()
{
    return {
        { "I 60 x 16", { Rect (0, 0, 60, 16) } },
        { "I 36 x 16", { Rect (0, 0, 36, 16) } },
        { "I 60 x 10, thin bar", { Rect (0, 0, 60, 10) } },
        { "I 48 x 12, sections", { Rect (0, 0, 48, 12) } },
        { "I 40 x 7.5, shallow", { Rect (0, 0, 40, 7.5) } },
        { "L 48 x 16 and 16 x 32", { Rect (0, 0, 48, 16), Rect (0, 16, 16, 48) } },
        { "U 60 x 16 with two 16 x 24 arms", { Rect (0, 0, 60, 16), Rect (0, 16, 16, 40), Rect (44, 16, 60, 40) } },
        { "O 50 x 50, 14 deep", { Rect (0, 0, 50, 50), Reversed (Rect (14, 14, 36, 36)) } },
        { "T 60 x 16 with a 16 x 24 stem", { Rect (0, 0, 60, 16), Rect (22, 16, 38, 40) } },
        { "+ 64 x 16 crossed by 16 x 64", { Rect (0, 24, 64, 40), Rect (24, 0, 40, 24), Rect (24, 40, 40, 64) } },
        { "L at 60 degrees", { Rect (0, 0, 44, 16), Turned (Rect (0, 0, 40, 16), 60, { 40, 0 }) } },
        { "I 50 x 16, slanted end", { { { 0, 0 }, { 50, 0 }, { 56, 16 }, { 0, 16 } } } },
        { "I 60 x 16 turned 23 degrees", { Turned (Rect (0, 0, 60, 16), 23, { 1000, 2000 }) } },
        { "I 60 x 14, facade skewed to 15.5", { { { 0, 0 }, { 60, 0 }, { 60, 15.5 }, { 0, 14 } } } },
        { "I stepped: 30 x 12 and 30 x 15", { Rect (0, 0, 30, 12), Rect (30, 0, 60, 15) } },
    };
}
std::string Report (const fs::Scheme& s)
{
    std::ostringstream out;
    out << s.typology << ": " << s.wings.size () << " wings, " << s.flats.size () << " flats, " << s.cores.size ()
        << " stairs\n";
    for (const auto& d : s.diagnostics)
        out << "  [" << static_cast<int> (d.level) << "] " << d.code << " " << d.text << "\n";
    return out.str ();
}
bool Has (const fs::Scheme& s, const char* code)
{
    return std::any_of (s.diagnostics.begin (), s.diagnostics.end (),
                        [&] (const fs::Diagnostic& d) { return d.code == code; });
}
fs::Vec Centre (const fs::Ring& ring)
{
    fs::Vec c;
    for (const auto& p : ring)
        c.x += p.x / static_cast<double> (ring.size ()), c.y += p.y / static_cast<double> (ring.size ());
    return c;
}
double Unassigned (const fs::Scheme& s)
{
    double a = 0;
    for (const auto& u : s.unassigned)
        a += std::abs (fs::Area (u.shape));
    return a;
}
// Same plan: what a user would see as the same floor.
void ExpectSame (const fs::Scheme& a, const fs::Scheme& b, const char* name)
{
    EXPECT_EQ (a.typology, b.typology) << name;
    ASSERT_EQ (a.wings.size (), b.wings.size ()) << name << "\n" << Report (a) << Report (b);
    EXPECT_EQ (a.flats.size (), b.flats.size ()) << name << "\n" << Report (a) << Report (b);
    EXPECT_EQ (a.cores.size (), b.cores.size ()) << name;
    EXPECT_EQ (a.corridors.size (), b.corridors.size ()) << name;
    EXPECT_EQ (a.ok, b.ok) << name << "\n" << Report (a) << Report (b);
    EXPECT_NEAR (a.net, b.net, 0.01) << name;
    EXPECT_NEAR (a.circulation, b.circulation, 0.01) << name;
    EXPECT_NEAR (Unassigned (a), Unassigned (b), 0.01) << name;
    for (size_t i = 0; i < a.cores.size () && i < b.cores.size (); ++i) {
        EXPECT_NEAR (a.cores[i].centre.x, b.cores[i].centre.x, 1e-3) << name << " stair " << i;
        EXPECT_NEAR (a.cores[i].centre.y, b.cores[i].centre.y, 1e-3) << name << " stair " << i;
    }
}
} // namespace

// The parity the whole stage rests on: adopting the generator's wings changes nothing.
TEST (FloorSchemeSkeleton, TheWingsASchemeWasPlannedWithPlanItAgain)
{
    for (const auto& c : Cases ()) {
        const auto found = fs::Generate (c.outline, fp::Default ());
        EXPECT_FALSE (found.authored) << c.name;
        const auto design = fe::AdoptSkeleton (found, {});
        ASSERT_EQ (design.pins.wings.size (), found.wings.size ()) << c.name;
        const auto again = fs::Generate (c.outline, fp::Default (), design.pins);
        EXPECT_TRUE (again.authored) << c.name;
        ExpectSame (found, again, c.name);
        for (size_t i = 0; i < again.wings.size (); ++i)
            EXPECT_EQ (again.wings[i].id, i + 1) << c.name << ": a wing keeps the name its pin has";
    }
}

// Saved to the millimetre and read back, an adopted skeleton still plans a floor without error.
TEST (FloorSchemeSkeleton, ASavedSkeletonPlansTheFloorItWasSavedFrom)
{
    for (const auto& c : Cases ()) {
        const auto found = fs::Generate (c.outline, fp::Default ());
        fe::Designs designs, read;
        designs.floors[0] = fe::AdoptSkeleton (found, {});
        std::string error;
        ASSERT_TRUE (fe::FromJson (fe::ToJson (designs), read, error)) << c.name << ": " << error;
        const auto again = fs::Generate (c.outline, fp::Default (), read.floors[0].pins);
        EXPECT_FALSE (Has (again, "skeleton.wing_outside")) << c.name << "\n" << Report (again);
        EXPECT_EQ (again.wings.size (), found.wings.size ()) << c.name;
        EXPECT_EQ (again.flats.size (), found.flats.size ()) << c.name << "\n" << Report (found) << Report (again);
        EXPECT_EQ (again.ok, found.ok) << c.name << "\n" << Report (again);
    }
}

TEST (FloorSchemeSkeleton, AnalyseStopsAtTheBands)
{
    for (const auto& c : Cases ()) {
        fs::Options o;
        o.shallow = fs::Access::CoreOnly; // one plan, as a session that has chosen has it
        const auto whole = fs::Generate (c.outline, fp::Default (), {}, o);
        const auto skeleton = fs::Analyse (c.outline, fp::Default (), {}, o);
        EXPECT_EQ (skeleton.stage, fs::Stage::Skeleton) << c.name;
        EXPECT_EQ (whole.stage, fs::Stage::Units) << c.name;
        EXPECT_TRUE (skeleton.flats.empty ()) << c.name;
        EXPECT_EQ (skeleton.typology, whole.typology) << c.name;
        EXPECT_EQ (skeleton.wings.size (), whole.wings.size ()) << c.name;
        EXPECT_EQ (skeleton.cores.size (), whole.cores.size ()) << c.name;
        EXPECT_EQ (skeleton.corridors.size (), whole.corridors.size ()) << c.name;
        EXPECT_EQ (skeleton.bands.size (), whole.bands.size ()) << c.name;
        EXPECT_FALSE (skeleton.bands.empty ()) << c.name;
        EXPECT_TRUE (skeleton.ok) << c.name << "\n" << Report (skeleton);
    }
}

TEST (FloorSchemeSkeleton, AWingTheFloorCannotTakeIsSaidAndLeftOut)
{
    const std::vector<fs::Ring> bar { Rect (0, 0, 60, 16) };
    struct Bad {
        fs::Pins::Wing wing;
        const char* code;
    };
    const Bad bad[] = {
        { { 7, { 10, 8 }, { 70, 8 }, 16 }, "skeleton.wing_outside" },  // 10 m past the gable
        { { 7, { 0, 2 }, { 60, 2 }, 4 }, "skeleton.wing_shallow" },    // 4 m deep
        { { 7, { 0, 8 }, { 5, 8 }, 5.5 }, "skeleton.wing_short" },     // 5 m by 5.5 m
        { { 7, { 30, 8 }, { 30, 8 }, 16 }, "skeleton.wing_malformed" } // no length
    };
    for (const auto& b : bad) {
        fs::Pins pins;
        pins.wings = { { 1, { 0, 8 }, { 30, 8 }, 16 }, b.wing };
        const auto s = fs::Generate (bar, fp::Default (), pins);
        EXPECT_TRUE (Has (s, b.code)) << b.code << "\n" << Report (s);
        EXPECT_FALSE (s.ok) << b.code;
        ASSERT_EQ (s.wings.size (), 1u) << b.code << ": the wing that fits is planned, the other left out";
        EXPECT_EQ (s.wings[0].id, 1u);
        EXPECT_FALSE (s.flats.empty ()) << b.code;
    }
    // Two wings over each other: the first stands, the second is said.
    fs::Pins pins;
    pins.wings = { { 1, { 0, 8 }, { 40, 8 }, 16 }, { 2, { 30, 8 }, { 60, 8 }, 16 } };
    const auto s = fs::Generate (bar, fp::Default (), pins);
    EXPECT_TRUE (Has (s, "skeleton.wing_overlap")) << Report (s);
    ASSERT_EQ (s.wings.size (), 1u);
    // Nothing fits: no plan, and it says so.
    pins.wings = { { 1, { 100, 8 }, { 160, 8 }, 16 } };
    const auto none = fs::Generate (bar, fp::Default (), pins);
    EXPECT_TRUE (Has (none, "skeleton.empty")) << Report (none);
    EXPECT_TRUE (none.flats.empty ());
}

// The user's skeleton is not the generator's: an L it reads as a long bar and an arm is planned as
// the user drew it, the long wing up the other way.
TEST (FloorSchemeSkeleton, AnLIsPlannedOnTheWingsTheUserDrew)
{
    const std::vector<fs::Ring> l { Rect (0, 0, 48, 16), Rect (0, 16, 16, 48) };
    const auto found = fs::Generate (l, fp::Default ());
    ASSERT_EQ (found.wings.size (), 2u) << Report (found);
    fs::Pins pins;
    pins.wings = { { 1, { 8, 0 }, { 8, 48 }, 16 }, { 2, { 16, 8 }, { 48, 8 }, 16 } };
    const auto drawn = fs::Generate (l, fp::Default (), pins);
    ASSERT_EQ (drawn.wings.size (), 2u) << Report (drawn);
    EXPECT_TRUE (drawn.authored);
    EXPECT_NEAR (drawn.wings[0].length, 48, 1e-6);
    EXPECT_NEAR (drawn.wings[1].length, 32, 1e-6);
    EXPECT_EQ (drawn.wings[1].parent, 0);
    EXPECT_FALSE (Has (drawn, "skeleton.wing_outside")) << Report (drawn);
    EXPECT_FALSE (drawn.flats.empty ()) << Report (drawn);
    EXPECT_FALSE (drawn.cores.empty ());
    // Every wing's floor is some flat's, corridor's or stair's: little is left over.
    EXPECT_LT (Unassigned (drawn), 0.05 * drawn.gross) << Report (drawn);
}

// A long bar cut into two wings by the user gets a stair in each.
TEST (FloorSchemeSkeleton, ABarDrawnAsTwoWingsHasAStairInEach)
{
    const std::vector<fs::Ring> bar { Rect (0, 0, 60, 16) };
    fs::Pins pins;
    pins.wings = { { 1, { 0, 8 }, { 34, 8 }, 16 }, { 2, { 34, 8 }, { 60, 8 }, 16 } };
    const auto s = fs::Generate (bar, fp::Default (), pins);
    ASSERT_EQ (s.wings.size (), 2u) << Report (s);
    for (const auto& w : s.wings) {
        const auto stairs = std::count_if (s.cores.begin (), s.cores.end (), [&] (const fs::Core& c) {
            return fe::WingAt (s, c.centre) == static_cast<int> (&w - s.wings.data ());
        });
        EXPECT_GE (stairs, 1) << "wing " << w.id << "\n" << Report (s);
    }
    EXPECT_FALSE (s.flats.empty ());
}

TEST (FloorSchemeSkeleton, ACorridorRunsWhereItIsPinnedAndAsWide)
{
    const std::vector<fs::Ring> bar { Rect (0, 0, 60, 18) };
    const auto plain = fs::Generate (bar, fp::Default ());
    ASSERT_FALSE (plain.corridors.empty ()) << Report (plain);
    EXPECT_NEAR (Centre (plain.corridors[0].shape).y, 9, 1e-6);

    fs::Pins pins;
    pins.corridors = { { { 30, 10.5 }, 2.4 } };
    const auto moved = fs::Generate (bar, fp::Default (), pins);
    ASSERT_FALSE (moved.corridors.empty ()) << Report (moved);
    for (const auto& c : moved.corridors) {
        EXPECT_NEAR (Centre (c.shape).y, 10.5, 1e-6) << Report (moved);
        const double run = std::hypot (c.axis[1].x - c.axis[0].x, c.axis[1].y - c.axis[0].y);
        EXPECT_NEAR (std::abs (fs::Area (c.shape)) / run, 2.4, 1e-6);
    }
    EXPECT_TRUE (moved.ok) << Report (moved);
    EXPECT_FALSE (Has (moved, "pin.corridor_moved"));
    EXPECT_FALSE (Has (moved, "pin.corridor_ignored"));

    // Too near a facade for a row of flats: brought back to where one fits, and said.
    pins.corridors = { { { 30, 16 }, 0 } };
    const auto near = fs::Generate (bar, fp::Default (), pins);
    ASSERT_FALSE (near.corridors.empty ());
    EXPECT_TRUE (Has (near, "pin.corridor_moved")) << Report (near);
    EXPECT_NEAR (Centre (near.corridors[0].shape).y, 18 - fs::kRowDepthMin - 0.9, 1e-6) << Report (near);

    // A wing of sections round stairs has no corridor to move: the pin is said, not lost quietly.
    const std::vector<fs::Ring> thin { Rect (0, 0, 48, 12) };
    fs::Options sections;
    sections.shallow = fs::Access::CoreOnly;
    pins.corridors = { { { 24, 6 }, 2.0 } };
    const auto ignored = fs::Generate (thin, fp::Default (), pins, sections);
    EXPECT_TRUE (Has (ignored, "pin.corridor_ignored")) << Report (ignored);
}

TEST (FloorSchemeSkeleton, AWingEditIsMadeOrRefusedWithTheMeasure)
{
    const std::vector<fs::Ring> bar { Rect (0, 0, 60, 16) };
    const auto s = fs::Generate (bar, fp::Default ());
    ASSERT_EQ (s.wings.size (), 1u);
    std::string why;

    // Off the floor: refused, the design untouched (not even adopted).
    auto d = fe::MoveWing (s, {}, 0, { 5, 0 }, &why);
    EXPECT_FALSE (fe::Authored (d));
    EXPECT_NE (why.find ("outside the floor"), std::string::npos) << why;

    // An end pulled in 12 m: the wing is 48 m, and its floor beyond is nobody's.
    why.clear ();
    d = fe::ResizeWing (s, {}, 0, { 60, 8 }, -12, &why);
    ASSERT_TRUE (fe::Authored (d)) << why;
    const auto shorter = fs::Generate (bar, fp::Default (), d.pins);
    ASSERT_EQ (shorter.wings.size (), 1u) << Report (shorter);
    EXPECT_NEAR (shorter.wings[0].length, 48, 1e-6);
    EXPECT_GT (Unassigned (shorter), 12 * 16 * 0.5) << Report (shorter);

    // A long side pushed in 3 m: 13 m deep; pushed in 12 m: under the least depth, refused.
    d = fe::ResizeWing (s, {}, 0, { 30, 16 }, -3, &why);
    ASSERT_TRUE (fe::Authored (d));
    EXPECT_NEAR (d.pins.wings[0].depth, 13, 1e-9);
    EXPECT_NEAR (d.pins.wings[0].a.y, 6.5, 1e-9);
    why.clear ();
    d = fe::ResizeWing (s, {}, 0, { 30, 16 }, -12, &why);
    EXPECT_FALSE (fe::Authored (d));
    EXPECT_NE (why.find ("at least"), std::string::npos) << why;

    // The last wing stays.
    why.clear ();
    d = fe::RemoveWing (s, {}, 0, &why);
    EXPECT_FALSE (fe::Authored (d));
    EXPECT_FALSE (why.empty ());

    // Released, the generator searches again.
    d = fe::ReleaseSkeleton (fe::AdoptSkeleton (s, {}));
    EXPECT_FALSE (fe::Authored (d));
}

TEST (FloorSchemeSkeleton, AMovedWingTakesItsPinsWithIt)
{
    // Two bars 4 m apart on one floor; the second is free to slide along itself.
    const std::vector<fs::Ring> floor { Rect (0, 0, 40, 16), Rect (0, 20, 60, 36) };
    const auto s = fs::Generate (floor, fp::Default ());
    ASSERT_EQ (s.wings.size (), 2u) << Report (s);
    const int shortWing = s.wings[0].length < s.wings[1].length ? 0 : 1;
    fe::Design d;
    d.pins.cores = { { { 20, 12 }, 0, 0 } };    // in the short bar
    d.pins.corridors = { { { 30, 28 }, 2.0 } }; // in the long one
    // The short bar cannot leave its floor; the test moves what it can: add a wing, then remove it.
    std::string why;
    auto moved = fe::MoveWing (s, d, shortWing, { 0, 0.1 }, &why);
    EXPECT_EQ (moved, d) << "0.1 m past its floor is within the slack only when the area is; here refused or kept";
    auto added = fe::AddWing (s, d, { 0, 8 }, { 40, 8 }, 16, &why);
    EXPECT_EQ (added, d) << "a wing over another is refused";
    EXPECT_NE (why.find ("over another wing"), std::string::npos) << why;
    auto fewer = fe::RemoveWing (s, d, shortWing, &why);
    ASSERT_EQ (fewer.pins.wings.size (), 1u);
    const auto one = fs::Generate (floor, fp::Default (), fewer.pins);
    ASSERT_EQ (one.wings.size (), 1u) << Report (one);
    EXPECT_GT (Unassigned (one), 0.9 * 40 * 16) << "the removed wing's floor is nobody's\n" << Report (one);
}

TEST (FloorSchemeSkeleton, ACorridorEditIsRefusedWhereItLeavesNoRowOfFlats)
{
    const std::vector<fs::Ring> bar { Rect (0, 0, 60, 18) };
    const auto s = fs::Generate (bar, fp::Default ());
    ASSERT_EQ (s.wings.size (), 1u);
    ASSERT_EQ (s.wings[0].access, fs::Access::Centre);
    std::string why;
    auto d = fe::SetCorridor (s, {}, 0, { 12, 10 }, 2.0, &why);
    ASSERT_EQ (d.pins.corridors.size (), 1u) << why;
    EXPECT_NEAR (d.pins.corridors[0].at.x, 30, 1e-9) << "kept on the wing's middle along it";
    EXPECT_NEAR (d.pins.corridors[0].at.y, 10, 1e-9);
    // Again elsewhere: one pin for the wing, not two.
    d = fe::SetCorridor (s, d, 0, { 40, 8.5 }, 0, &why);
    ASSERT_EQ (d.pins.corridors.size (), 1u);
    EXPECT_NEAR (d.pins.corridors[0].at.y, 8.5, 1e-9);

    why.clear ();
    const auto refused = fe::SetCorridor (s, d, 0, { 30, 15 }, 0, &why);
    EXPECT_EQ (refused, d);
    EXPECT_NE (why.find ("at least 5.5 m"), std::string::npos) << why;
    why.clear ();
    EXPECT_EQ (fe::SetCorridor (s, d, 0, { 30, 9 }, 0.9, &why), d);
    EXPECT_NE (why.find ("1.2 to 3.0 m"), std::string::npos) << why;
    EXPECT_TRUE (fe::ResetCorridor (s, d, 0).pins.corridors.empty ());

    // Sections round stairs: no corridor to set.
    fs::Options o;
    o.shallow = fs::Access::CoreOnly;
    const auto thin = fs::Generate ({ Rect (0, 0, 48, 12) }, fp::Default (), {}, o);
    why.clear ();
    EXPECT_TRUE (fe::SetCorridor (thin, {}, 0, { 24, 6 }, 0, &why).pins.corridors.empty ());
    EXPECT_NE (why.find ("no corridor"), std::string::npos) << why;
}

TEST (FloorSchemeSkeleton, WingsAndCorridorsAreSavedAndADesignWithoutThemStaysVersionOne)
{
    fe::Designs designs, read;
    designs.floors[2].pins.wings = { { 1, { 0, 8 }, { 34.125, 8 }, 16, false },
                                     { 4, { 42, 16 }, { 42, 40 }, 16, true } };
    designs.floors[2].pins.corridors = { { { 17, 9.25 }, 2.4 }, { { 42, 30 }, 0 } };
    const auto text = fe::ToJson (designs);
    EXPECT_NE (text.find ("\"version\":2"), std::string::npos) << text;
    std::string error;
    ASSERT_TRUE (fe::FromJson (text, read, error)) << error;
    EXPECT_EQ (read.floors[2], designs.floors[2]);

    fe::Designs plain;
    plain.floors[0].pins.cores = { { { 18, 8 }, 4.5, 4.2 } };
    EXPECT_NE (fe::ToJson (plain).find ("\"version\":1"), std::string::npos);

    // Malformed wings are refused, never half read.
    for (const char* bad : { R"({"version":2,"floors":[{"story":0,"design":{"wings":[[0,0,8,30,8,16,0]]}}]})",
                             R"({"version":2,"floors":[{"story":0,"design":{"wings":[[1,0,8,30,8,-1,0]]}}]})",
                             R"({"version":2,"floors":[{"story":0,"design":{"wings":[[1,0,8,30,8,16,2]]}}]})",
                             R"({"version":2,"floors":[{"story":0,"design":{"wings":[[1,0,8,30,8,16]]}}]})",
                             R"({"version":3,"floors":[]})" })
        EXPECT_FALSE (fe::FromJson (bad, read, error)) << bad;
}

// "Add a wing here": the largest wing the free floor under the pointer takes, square to the
// skeleton; where the floor is some wing's already, or too small for one, it is refused.
TEST (FloorSchemeSkeleton, AWingIsAddedToTheFreeFloorUnderAPoint)
{
    const std::vector<fs::Ring> bar { Rect (0, 0, 60, 16) };
    fs::Pins pins;
    pins.wings = { { 1, { 0, 8 }, { 40, 8 }, 16 } };
    const auto s = fs::Generate (bar, fp::Default (), pins);
    ASSERT_EQ (s.wings.size (), 1u) << Report (s);
    fe::Design d;
    d.pins = pins;
    std::string why;
    const auto added = fe::AddWingAt (s, d, { 50, 8 }, &why);
    ASSERT_EQ (added.pins.wings.size (), 2u) << why;
    EXPECT_EQ (added.pins.wings[1].id, 2u);
    const auto both = fs::Generate (bar, fp::Default (), added.pins);
    ASSERT_EQ (both.wings.size (), 2u) << Report (both);
    EXPECT_NEAR (std::abs (fs::Area (both.wings[1].rect)), 20 * 16, 1.0) << "the 20 x 16 m the first wing left";
    EXPECT_LT (Unassigned (both), 1.0) << Report (both);
    why.clear ();
    EXPECT_EQ (fe::AddWingAt (s, d, { 20, 8 }, &why), d);
    EXPECT_NE (why.find ("already belongs to a wing"), std::string::npos) << why;
}
