#include "Edit/EditPathCrossing.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;

namespace {

// Path A runs east along y = 0 from +0 m to +2 m over 20 m (1.5 m wide, 2 % crown); B is
// added through a session, as the editor adds it.
struct Crossing {
    ed::Session s;
    ed::Id a = ed::kNoId, b = ed::kNoId;

    Crossing ()
    {
        ed::Document doc;
        a = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 2 } }, 1.5);
        s = ed::Session (doc);
    }
    ed::Id Add (std::vector<ed::Vec3> points)
    {
        ed::Id made = ed::kNoId;
        EXPECT_TRUE (s.Apply ([&] (ed::Document& d, std::string&) {
            made = ed::AddPath (d, points, 1.5);
            return true;
        })) << s.Message ();
        return made;
    }
    const ed::Path& P (ed::Id id) const
    {
        return *ed::FindPath (s.Doc (), id);
    }
    const ed::GradeBreak* Junction (ed::Id path, ed::Id other) const
    {
        for (const auto& g : P (path).profile)
            if (g.crossing == other)
                return &g;
        return nullptr;
    }
};

} // namespace

TEST (EditPathCrossing, CentrelinesThatCrossAreAJunction)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 } }, 1.5);
    ed::AddPath (doc, { { 10, -10, 0 }, { 10, 10, 0 } }, 1.5);
    const auto j = ed::FindJunctions (doc.paths);
    ASSERT_EQ (j.size (), 1u);
    EXPECT_NEAR (j[0].stationA, 10.0, 1e-6);
    EXPECT_NEAR (j[0].stationB, 10.0, 1e-6);
    EXPECT_NEAR (j[0].rise, 0.0, 1e-12);
}

TEST (EditPathCrossing, ACrossingPathTakesTheOlderPathsHeight)
{
    Crossing c;
    const auto before = c.P (c.a).profile;
    const ed::Id b = c.Add ({ { 10, -10, 5 }, { 10, 10, 5 } });
    const auto* onA = c.Junction (c.a, b);
    const auto* onB = c.Junction (b, c.a);
    ASSERT_NE (onA, nullptr);
    ASSERT_NE (onB, nullptr);
    EXPECT_NEAR (onA->station, 10.0, 1e-6);
    EXPECT_NEAR (onA->z, 1.0, 1e-9); // on A's own grade: A keeps its shape
    EXPECT_EQ (c.P (c.a).profile.size (), before.size () + 1);
    EXPECT_NEAR (onB->station, 10.0, 1e-6);
    EXPECT_NEAR (onB->z, 1.0, 1e-9); // B meets A
}

TEST (EditPathCrossing, AnElevationSetOnEitherPathIsCarriedToTheOther)
{
    Crossing c;
    const ed::Id b = c.Add ({ { 10, -10, 5 }, { 10, 10, 5 } });
    const ed::Id onB = c.Junction (b, c.a)->id;
    ASSERT_TRUE (
        c.s.Apply ([&] (ed::Document& d, std::string& e) { return ed::SetPathElevation (d, b, onB, 1.6, e); }));
    EXPECT_NEAR (c.Junction (c.a, b)->z, 1.6, 1e-9);
    const ed::Id onA = c.Junction (c.a, b)->id;
    ASSERT_TRUE (
        c.s.Apply ([&] (ed::Document& d, std::string& e) { return ed::SetPathElevation (d, c.a, onA, 0.4, e); }));
    EXPECT_NEAR (c.Junction (b, c.a)->z, 0.4, 1e-9);
}

TEST (EditPathCrossing, TheJunctionFollowsAMovedPathAndGoesWhenTheyPart)
{
    Crossing c;
    const ed::Id b = c.Add ({ { 10, -10, 5 }, { 10, 10, 5 } });
    const ed::Id kept = c.Junction (c.a, b)->id;
    ASSERT_TRUE (c.s.Apply ([&] (ed::Document& d, std::string& e) {
        return ed::TransformPathParts (d, { { b, ed::Part::Entity, ed::kNoId } }, { 4, 0, 0 }, e);
    })) << c.s.Message ();
    ASSERT_NE (c.Junction (c.a, b), nullptr);
    EXPECT_EQ (c.Junction (c.a, b)->id, kept); // the same break, moved
    EXPECT_NEAR (c.Junction (c.a, b)->station, 14.0, 1e-6);
    EXPECT_NEAR (c.Junction (b, c.a)->z, c.Junction (c.a, b)->z, 1e-9);
    const size_t breaks = c.P (c.a).profile.size ();
    ASSERT_TRUE (c.s.Apply ([&] (ed::Document& d, std::string& e) {
        return ed::TransformPathParts (d, { { b, ed::Part::Entity, ed::kNoId } }, { 30, 0, 0 }, e);
    }));
    EXPECT_EQ (c.Junction (c.a, b), nullptr);
    EXPECT_EQ (c.P (c.a).profile.size (), breaks - 1);
}

TEST (EditPathCrossing, APathEndingOnAnotherMeetsItsSurface)
{
    Crossing c;
    // A T: B runs south and stops half a metre left of A's centreline, on A's surface.
    const ed::Id b = c.Add ({ { 10, 10, 5 }, { 10, 0.5, 5 } });
    const auto junctions = ed::FindJunctions (c.s.Doc ().paths);
    ASSERT_EQ (junctions.size (), 1u);
    EXPECT_LT (junctions[0].rise, 0.0); // the crown falls towards A's edge
    const auto* end = c.Junction (b, c.a);
    ASSERT_NE (end, nullptr);
    EXPECT_EQ (end->id, c.P (b).profile.back ().id); // B's own end break
    EXPECT_NEAR (end->z, c.Junction (c.a, b)->z + junctions[0].rise, 1e-9);
    EXPECT_NEAR (c.Junction (c.a, b)->station, 10.0, 1e-3);
}

TEST (EditPathCrossing, AJunctionsBreakStaysWhileThePathsCross)
{
    Crossing c;
    const ed::Id b = c.Add ({ { 10, -10, 5 }, { 10, 10, 5 } });
    const ed::Id onA = c.Junction (c.a, b)->id;
    EXPECT_FALSE (c.s.Apply ([&] (ed::Document& d, std::string& e) { return ed::DeletePathBreak (d, c.a, onA, e); }));
    EXPECT_NE (c.s.Message ().find ("cross"), std::string::npos);
    EXPECT_FALSE (
        c.s.Apply ([&] (ed::Document& d, std::string& e) { return ed::MovePathBreak (d, c.a, onA, 4.0, e); }));
}
