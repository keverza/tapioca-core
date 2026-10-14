#include "Edit/EditGizmo.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditSnap.hpp"
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
// Two non-adjacent segments of an edge cross in plan.
bool FoldsOver (const std::vector<ed::Vec3>& e)
{
    for (size_t i = 0; i + 1 < e.size (); ++i)
        for (size_t j = i + 2; j + 1 < e.size (); ++j) {
            const double rx = e[i + 1].x - e[i].x, ry = e[i + 1].y - e[i].y;
            const double sx = e[j + 1].x - e[j].x, sy = e[j + 1].y - e[j].y;
            const double den = rx * sy - ry * sx;
            if (std::abs (den) < 1.0e-12)
                continue;
            const double qx = e[j].x - e[i].x, qy = e[j].y - e[i].y;
            const double u = (qx * sy - qy * sx) / den, v = (qx * ry - qy * rx) / den;
            if (u > 1.0e-6 && u < 1.0 - 1.0e-6 && v > 1.0e-6 && v < 1.0 - 1.0e-6)
                return true;
        }
    return false;
}

bool Holds (const std::vector<ed::Vec3>& edge, ed::Vec3 p)
{
    for (const auto& q : edge)
        if (ed::DistanceXY (p, q) < 1.0e-6)
            return true;
    return false;
}
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

TEST (EditPath, OutlineStepsAtARangeBoundary)
{
    Footpath f;
    std::string error;
    ed::Id wide = 0;
    ASSERT_TRUE (ed::SplitPathSection (f.doc, f.path, 20.0, error, &wide)) << error;
    ASSERT_TRUE (ed::SetPathWidth (f.doc, f.path, wide, 2.0, 2.0, error));
    ed::PathOutline o;
    ASSERT_TRUE (ed::Outline (f.P (), 1.0, o, error)) << error;
    ASSERT_EQ (o.points.size (), 3u);
    ASSERT_EQ (o.breaks.size (), 2u);
    size_t at = 0;
    while (o.stations[at] < 20.0 - 1e-6)
        ++at;
    EXPECT_NEAR (o.left[at - 1].y, 1.0, 1e-9); // just before: the old width
    EXPECT_NEAR (o.left[at].y, 2.0, 1e-9);     // at the boundary: the new one
    EXPECT_EQ (o.range[at], wide);
}

TEST (EditPath, PathsSnapAndPick)
{
    Footpath f;
    ed::SnapQuery q;
    q.cursor = { 0.05, 0.03, 0 };
    q.tolerance = 0.1;
    const auto start = ed::Snap (f.doc, {}, q);
    EXPECT_EQ (start.kind, ed::SnapKind::Endpoint);
    EXPECT_EQ (start.source.entity, f.path);
    q.cursor = { 10, 0.98, 0 };
    const auto edge = ed::Snap (f.doc, {}, q);
    EXPECT_EQ (edge.kind, ed::SnapKind::Nearest);
    EXPECT_EQ (edge.source.part, ed::Part::Range);
    q.exclude = { { f.path, ed::Part::PathPoint, f.P ().points[0].id } };
    q.cursor = { 0.05, 0.03, 0 };
    EXPECT_FALSE (ed::Snap (f.doc, {}, q).snapped); // a moving path does not snap to itself

    ed::PickView plan;
    plan.toScreen = [] (ed::Vec3 p, double& x, double& y) {
        x = p.x * 20.0;
        y = p.y * 20.0;
        return true;
    };
    const auto hits = ed::Pick (f.doc, plan, 200, 19, ed::kPickAll, 6);
    ASSERT_FALSE (hits.empty ());
    EXPECT_EQ (hits[0].ref.part, ed::Part::Range); // the left edge at y = 1
    const auto centre = ed::Pick (f.doc, plan, 200, 1, ed::kPickEntity, 6);
    ASSERT_EQ (centre.size (), 1u);
    EXPECT_EQ (centre[0].ref.entity, f.path);
    const auto corner = ed::Pick (f.doc, plan, 0, 0, ed::kPickVertex, 6);
    ASSERT_EQ (corner.size (), 2u); // the first control point and the first grade break
    ed::Selection sel;
    sel.Marquee (f.doc, plan, -20, -20, 900, 700, ed::kPickEntity, false);
    ASSERT_EQ (sel.Items ().size (), 1u);
    EXPECT_EQ (sel.Items ()[0].entity, f.path);
}

TEST (EditPath, SharpCornerIsMitredAndItsEdgesNeverCross)
{
    // The live run's footpath: a sharp left turn at (25, -10), 1.8 m wide.
    ed::Document doc;
    const ed::Id id = ed::AddPath (doc, { { 0, -10, 0 }, { 25, -10, 0 }, { 25, 5, 0 } }, 1.8);
    ed::PathOutline o;
    std::string error;
    ASSERT_TRUE (ed::Outline (*ed::FindPath (doc, id), 0.5, o, error)) << error;
    EXPECT_FALSE (FoldsOver (o.left));
    EXPECT_FALSE (FoldsOver (o.right));
    EXPECT_TRUE (Holds (o.right, { 25.9, -10.9, 0 })); // the outer mitre
    EXPECT_TRUE (Holds (o.left, { 24.1, -9.1, 0 }));   // the inner corner
    for (const auto& p : o.left)                       // nothing of the inner edge runs past its corner
        EXPECT_TRUE (p.x <= 24.1 + 1.0e-9 && p.y >= -9.1 - 1.0e-9) << p.x << ", " << p.y;
    EXPECT_EQ (o.left.size (), o.stations.size ());
    EXPECT_EQ (o.right.size (), o.stations.size ());
}

TEST (EditPath, BendTighterThanTheWidthHasItsFoldCutOut)
{
    ed::Document doc;
    const ed::Id id = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 }, { 20, 20, 0 } }, 4.0);
    std::string error;
    ASSERT_TRUE (ed::SetPathRadius (doc, id, ed::FindPath (doc, id)->points[1].id, 1.0, error)) << error;
    ed::PathOutline o;
    ASSERT_TRUE (ed::Outline (*ed::FindPath (doc, id), 0.25, o, error)) << error;
    EXPECT_FALSE (FoldsOver (o.left)); // 2 m to the left of a 1 m arc
    EXPECT_FALSE (FoldsOver (o.right));
}

TEST (EditPath, HairpinCornerIsBevelled)
{
    ed::Document doc;
    const ed::Id id = ed::AddPath (doc, { { 0, 0, 0 }, { 30, 0, 0 }, { 10, 6, 0 } }, 1.0);
    ed::PathOutline o;
    std::string error;
    ASSERT_TRUE (ed::Outline (*ed::FindPath (doc, id), 0.5, o, error)) << error;
    size_t atCorner = 0;
    for (size_t i = 0; i < o.stations.size (); ++i)
        if (std::abs (o.stations[i] - 30.0) < 1.0e-6) {
            ++atCorner; // each leg's own outer offset, half a width from the corner: no mitre spike
            EXPECT_NEAR (ed::DistanceXY (o.right[i], { 30, 0, 0 }), 0.5, 1.0e-9);
        }
    EXPECT_EQ (atCorner, 2u);
    EXPECT_FALSE (FoldsOver (o.right));
}
