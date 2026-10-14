#include "Edit/EditGizmo.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
// An L-shaped footpath: 40 m east, then 30 m north, a 5 m bend, 2 m wide,
// rising from 100.0 to 101.4.
struct Footpath {
    ed::Document doc;
    ed::Id path = 0;
    std::string error;
    Footpath ()
    {
        path = ed::AddPath (doc, { { 0, 0, 100.0 }, { 40, 0, 0 }, { 40, 30, 101.4 } }, 2.0, "footpath");
        EXPECT_TRUE (ed::SetPathRadius (doc, path, P ().points[1].id, 5.0, error)) << error;
    }
    const ed::Path& P () const
    {
        return *ed::FindPath (doc, path);
    }
    ed::AlignmentGeometry G () const
    {
        ed::AlignmentGeometry g;
        std::string why;
        EXPECT_TRUE (ed::BuildAlignment (P ().points, g, why)) << why;
        return g;
    }
    void Valid ()
    {
        std::string why;
        EXPECT_TRUE (ed::Validate (doc, why)) << why;
    }
};
} // namespace

TEST (EditPath, NewPathIsAValidDefinition)
{
    Footpath f;
    f.Valid ();
    const auto& p = f.P ();
    EXPECT_EQ (p.role, "footpath");
    ASSERT_EQ (p.profile.size (), 2u);
    EXPECT_DOUBLE_EQ (p.profile.back ().z, 101.4);
    EXPECT_NEAR (p.profile.back ().station, f.G ().length, 1e-9);
    ASSERT_EQ (p.sections.size (), 1u);
    EXPECT_DOUBLE_EQ (p.sections[0].widthLeft, 1.0);
}

TEST (EditPath, EvaluateGivesCentreAndCrossfallEdges)
{
    Footpath f;
    const auto g = f.G ();
    ed::StationFrame s;
    ASSERT_TRUE (ed::Evaluate (f.P (), g, 10.0, s));
    EXPECT_NEAR (s.centre.x, 10.0, 1e-9);
    EXPECT_NEAR (s.grade, 1.4 / g.length, 1e-12);
    EXPECT_NEAR (s.centre.z, 100.0 + 10.0 * 1.4 / g.length, 1e-12);
    // Heading east, left is north; a 2 % crown drops each edge 2 cm.
    EXPECT_NEAR (s.leftEdge.y, 1.0, 1e-9);
    EXPECT_NEAR (s.rightEdge.y, -1.0, 1e-9);
    EXPECT_NEAR (s.leftEdge.z, s.centre.z - 0.02, 1e-12);
}

TEST (EditPath, SampleStationsHitEveryFeature)
{
    Footpath f;
    std::string error;
    ed::Id brk = 0;
    ASSERT_TRUE (ed::InsertPathBreak (f.doc, f.path, 20.0, error, &brk)) << error;
    ASSERT_TRUE (ed::SetPathCurve (f.doc, f.path, brk, 6.0, error)) << error;
    ASSERT_TRUE (ed::SplitPathSection (f.doc, f.path, 50.0, error)) << error;
    const auto g = f.G ();
    const auto s = ed::SampleStations (f.P (), g, 2.0);
    const auto has = [&] (double x) {
        for (double v : s)
            if (std::abs (v - x) < 1e-9)
                return true;
        return false;
    };
    for (double x : { 0.0, 17.0, 20.0, 23.0, 35.0, 50.0, g.length })
        EXPECT_TRUE (has (x)) << x;
    for (size_t i = 0; i + 1 < s.size (); ++i)
        EXPECT_LE (s[i + 1] - s[i], 2.0 + 1e-9);
}

TEST (EditPath, MovingAPointRestationsAnchorsButNeverHeights)
{
    Footpath f;
    std::string error;
    ed::Id brk = 0;
    ASSERT_TRUE (ed::InsertPathBreak (f.doc, f.path, 30.0, error, &brk)) << error;
    const ed::Id corner = f.P ().points[1].id;
    ASSERT_TRUE (ed::AnchorBreak (f.doc, f.path, brk, corner, error)) << error;
    const double z = f.P ().profile[1].z;
    ASSERT_TRUE (ed::MovePathPoint (f.doc, f.path, corner, { 50, 0, 0 }, error)) << error;
    const auto g = f.G ();
    EXPECT_NEAR (f.P ().profile[1].station, g.pointStations[1], 1e-9);
    EXPECT_DOUBLE_EQ (f.P ().profile[1].z, z);
    EXPECT_NEAR (f.P ().profile.back ().station, g.length, 1e-9);
    f.Valid ();
    // A move that would fold the plan back is refused and leaves the path alone.
    const auto before = f.P ();
    EXPECT_FALSE (ed::MovePathPoint (f.doc, f.path, corner, { 40, 30, 0 }, error));
    EXPECT_TRUE (f.P () == before);
}

TEST (EditPath, FixedEntranceHoldsWhileGradesChange)
{
    Footpath f;
    std::string error;
    const double end = f.G ().length;
    ed::Id entrance = 0;
    ASSERT_TRUE (ed::FixElevationAt (f.doc, f.path, end, 101.0, "door:A1", error, &entrance)) << error;
    EXPECT_EQ (f.P ().profile.back ().binding, "door:A1");
    ASSERT_TRUE (ed::SetPathGrade (f.doc, f.path, 10.0, 2.0, error)) << error;
    EXPECT_DOUBLE_EQ (f.P ().profile.back ().z, 101.0);                // the fixed end held
    EXPECT_NEAR (f.P ().profile.front ().z, 101.0 - 0.02 * end, 1e-9); // the start moved
    EXPECT_FALSE (ed::SetPathElevation (f.doc, f.path, entrance, 99.0, error));
    ASSERT_TRUE (ed::UnfixBreak (f.doc, f.path, entrance, error));
    EXPECT_TRUE (ed::SetPathElevation (f.doc, f.path, entrance, 99.0, error));
}

TEST (EditPath, InsertAndDeleteControlPoints)
{
    Footpath f;
    std::string error;
    const double length = f.G ().length;
    ed::Id added = 0;
    ASSERT_TRUE (ed::InsertPathPoint (f.doc, f.path, 10.0, error, &added)) << error;
    ASSERT_EQ (f.P ().points.size (), 4u);
    EXPECT_EQ (f.P ().points[1].id, added);
    EXPECT_NEAR (f.G ().length, length, 1e-9); // on a straight leg: same shape
    ASSERT_TRUE (ed::DeletePathPoint (f.doc, f.path, added, error));
    ASSERT_TRUE (ed::DeletePathPoint (f.doc, f.path, f.P ().points[1].id, error));
    EXPECT_FALSE (ed::DeletePathPoint (f.doc, f.path, f.P ().points[0].id, error));
    f.Valid ();
}

TEST (EditPath, ProfileFollowsTheSurfaceBetweenFixedPoints)
{
    Footpath f;
    std::string error;
    const double end = f.G ().length;
    ASSERT_TRUE (ed::FixElevationAt (f.doc, f.path, end, 104.0, "street", error)) << error;
    // A terrain with a ridge across x = 20.
    const auto terrain = [] (double x, double, double& z) {
        z = 100.0 + 2.0 - std::abs (x - 20.0) * 0.1;
        return true;
    };
    ASSERT_TRUE (ed::ProfileFromSurface (f.doc, f.path, terrain, 1.0, 0.02, error)) << error;
    const auto& b = f.P ().profile;
    ASSERT_GE (b.size (), 3u);
    EXPECT_NEAR (b.front ().z, 100.0, 1e-9);
    EXPECT_DOUBLE_EQ (b.back ().z, 104.0);
    EXPECT_TRUE (b.back ().fixed);
    // The ridge is kept within one sample step (the stations are not whole metres).
    bool ridge = false;
    for (const auto& x : b)
        ridge |= std::abs (x.station - 20.0) <= 1.0 && std::abs (x.z - 102.0) <= 0.1;
    EXPECT_TRUE (ridge);
    f.Valid ();
}

TEST (EditPath, DraggedEdgeSetsTheWidthOfItsRange)
{
    Footpath f;
    std::string error;
    ASSERT_TRUE (ed::SplitPathSection (f.doc, f.path, 30.0, error)) << error;
    ASSERT_TRUE (ed::SetPathWidthFromPoint (f.doc, f.path, { 10, 1.6, 0 }, error)) << error;
    EXPECT_DOUBLE_EQ (f.P ().sections[0].widthLeft, 1.6);
    EXPECT_DOUBLE_EQ (f.P ().sections[1].widthLeft, 1.0); // the other range is untouched
    ASSERT_TRUE (ed::SetPathCrossfall (f.doc, f.path, f.P ().sections[1].id, 2.5, ed::Fall::Right, error));
    EXPECT_EQ (f.P ().sections[1].fall, ed::Fall::Right);
}

TEST (EditPath, GizmoMovesPathPartsThroughTheSession)
{
    Footpath f;
    ed::Session s (f.doc);
    const ed::Id corner = f.P ().points[1].id;
    ed::Transform east;
    east.translate = { 5, 0, 0 };
    ASSERT_TRUE (s.Apply (ed::TransformOperation ({ { f.path, ed::Part::PathPoint, corner } }, east))) << s.Message ();
    EXPECT_DOUBLE_EQ (ed::FindPath (s.Doc (), f.path)->points[1].p.x, 45.0);
    ed::Transform up;
    up.translate = { 0, 0, 0.5 };
    const ed::Id start = f.P ().profile.front ().id;
    ASSERT_TRUE (s.Apply (ed::TransformOperation ({ { f.path, ed::Part::Break, start } }, up))) << s.Message ();
    EXPECT_DOUBLE_EQ (ed::FindPath (s.Doc (), f.path)->profile.front ().z, 100.5);
    ed::Transform turn;
    turn.angle = 0.3;
    EXPECT_FALSE (s.Apply (ed::TransformOperation ({ { f.path, ed::Part::Entity, 0 } }, turn)));
    ASSERT_TRUE (s.Undo ());
    ASSERT_TRUE (s.Undo ());
    EXPECT_DOUBLE_EQ (ed::FindPath (s.Doc (), f.path)->points[1].p.x, 40.0);
}

TEST (EditPath, PathPartsAreTypedDimensions)
{
    Footpath f;
    const ed::Ref range { f.path, ed::Part::Range, f.P ().sections[0].id };
    const auto dims = ed::DimensionsFor (f.doc, range);
    ASSERT_EQ (dims.size (), 2u);
    std::string error;
    ASSERT_TRUE (ed::WriteDimension (dims[0], 3.0) (f.doc, error)) << error; // total width
    EXPECT_DOUBLE_EQ (f.P ().sections[0].widthLeft, 1.5);
    double v = 0;
    ASSERT_TRUE (ed::ReadDimension (f.doc, dims[1], v));
    EXPECT_DOUBLE_EQ (v, 2.0);
    const ed::Ref corner { f.path, ed::Part::PathPoint, f.P ().points[1].id };
    ASSERT_TRUE (ed::WriteDimension (ed::DimensionsFor (f.doc, corner)[0], 8.0) (f.doc, error)) << error;
    EXPECT_DOUBLE_EQ (f.P ().points[1].radius, 8.0);
    EXPECT_FALSE (ed::WriteDimension (ed::DimensionsFor (f.doc, corner)[0], 80.0) (f.doc, error)); // does not fit
}

TEST (EditPath, RefsAndValidationCoverPaths)
{
    Footpath f;
    ed::Vec3 p;
    ASSERT_TRUE (ed::PointOf (f.doc, { f.path, ed::Part::Break, f.P ().profile.front ().id }, p));
    EXPECT_NEAR (p.z, 100.0, 1e-12);
    EXPECT_FALSE (ed::Exists (f.doc, { f.path, ed::Part::Break, 9999 }));
    ed::Selection sel;
    sel.Set ({ { f.path, ed::Part::PathPoint, f.P ().points[0].id } });
    EXPECT_EQ (sel.Kind (), ed::SelectionKind::PathPoint);
    ed::Document broken = f.doc;
    broken.paths[0].sections[0].id = broken.paths[0].points[0].id;
    std::string error;
    EXPECT_FALSE (ed::Validate (broken, error));
    EXPECT_NE (error.find ("duplicate"), std::string::npos);
}
