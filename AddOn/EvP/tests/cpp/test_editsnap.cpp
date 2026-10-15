#include "Edit/EditSnap.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
ed::SnapQuery At (double x, double y, double tolerance = 0.1)
{
    ed::SnapQuery q;
    q.cursor = { x, y, 0 };
    q.tolerance = tolerance;
    return q;
}

void ExpectAt (const ed::SnapResult& r, double x, double y)
{
    EXPECT_NEAR (r.point.x, x, 1e-9);
    EXPECT_NEAR (r.point.y, y, 1e-9);
}
} // namespace

TEST (EditSnap, EndpointBeatsMidpointAndEdge)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    const auto r = ed::Snap (doc, {}, At (3.95, 0.05));
    EXPECT_EQ (r.kind, ed::SnapKind::Endpoint);
    ExpectAt (r, 4, 0);
    EXPECT_EQ (r.hint, "Endpoint");
    EXPECT_EQ (r.source.part, ed::Part::Vertex);

    const auto mid = ed::Snap (doc, {}, At (2.04, -0.03));
    EXPECT_EQ (mid.kind, ed::SnapKind::Midpoint);
    ExpectAt (mid, 2, 0);

    const auto edge = ed::Snap (doc, {}, At (1.0, 0.05));
    EXPECT_EQ (edge.kind, ed::SnapKind::Nearest);
    ExpectAt (edge, 1, 0);
}

TEST (EditSnap, NothingInReachLeavesTheCursor)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    const auto r = ed::Snap (doc, {}, At (2.0, 1.0));
    EXPECT_FALSE (r.snapped);
    ExpectAt (r, 2, 1);
}

TEST (EditSnap, CrossingSegmentsGiveAnIntersection)
{
    ed::Document doc;
    // Midpoints (3,3) and (3,1) stay out of reach of the crossing at (2,2).
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 6, 6, 0 } }, false);
    ed::AddPolyline (doc, { { 0, 4, 0 }, { 6, -2, 0 } }, false);
    const auto r = ed::Snap (doc, {}, At (2.05, 1.97));
    EXPECT_EQ (r.kind, ed::SnapKind::Intersection);
    ExpectAt (r, 2, 2);
    EXPECT_TRUE (r.guides.empty ()); // two real edges: nothing transient to draw
}

TEST (EditSnap, DraggedVertexIgnoresItselfAndItsSegments)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 } }, false);
    auto q = At (4.02, 0.01);
    q.exclude = { { e, ed::Part::Vertex, ed::Find (doc, e)->vertices[1].id } };
    q.modes = ed::kSnapAll & ~ed::kSnapAxis;
    const auto r = ed::Snap (doc, {}, q);
    EXPECT_FALSE (r.snapped);
}

TEST (EditSnap, PerpendicularFootFromTheAnchor)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
    auto q = At (3.05, 0.05);
    q.anchor = ed::Vec3 { 3, 5, 0 };
    q.modes = ed::kSnapPerpendicular | ed::kSnapNearest;
    const auto r = ed::Snap (doc, {}, q);
    EXPECT_EQ (r.kind, ed::SnapKind::Perpendicular);
    ExpectAt (r, 3, 0);
    ASSERT_EQ (r.guides.size (), 1u);
    EXPECT_NEAR (r.guides[0].from.y, 5.0, 1e-12);
}

TEST (EditSnap, AxisGuideThroughTheAnchor)
{
    ed::Document doc;
    auto q = At (5.0, 2.06);
    q.anchor = ed::Vec3 { 0, 2, 0 };
    const auto r = ed::Snap (doc, {}, q);
    EXPECT_EQ (r.kind, ed::SnapKind::Axis);
    EXPECT_EQ (r.hint, "X axis");
    ExpectAt (r, 5, 2);
    ASSERT_EQ (r.guides.size (), 1u);
}

TEST (EditSnap, AxisCrossingAnEdgeIsAnIntersection)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 5, -10, 0 }, { 5, 10, 0 } }, false);
    auto q = At (5.04, 3.03);
    q.anchor = ed::Vec3 { 0, 3, 0 };
    const auto r = ed::Snap (doc, {}, q);
    EXPECT_EQ (r.kind, ed::SnapKind::Intersection);
    ExpectAt (r, 5, 3);
    ASSERT_EQ (r.guides.size (), 1u); // the axis guide is drawn, the edge is not
    EXPECT_EQ (r.guides[0].kind, ed::SnapKind::Axis);
}

TEST (EditSnap, ExtensionPastASegmentEnd)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    const auto r = ed::Snap (doc, {}, At (5.5, 0.04));
    EXPECT_EQ (r.kind, ed::SnapKind::Extension);
    ExpectAt (r, 5.5, 0);
    ASSERT_EQ (r.guides.size (), 1u);
    EXPECT_NEAR (r.guides[0].from.x, 4.0, 1e-12);
    // Beyond the reach there is no guide.
    EXPECT_FALSE (ed::Snap (doc, {}, At (9.0, 0.04)).snapped);
}

TEST (EditSnap, HostGeometryNamesItsSource)
{
    ed::Document doc;
    ed::SnapContext ctx;
    ctx.points.push_back ({ { 7, 7, 1.2 }, "Door threshold" });
    ctx.segments.push_back ({ { 0, 10, 0 }, { 10, 10, 0 }, "Wall" });
    const auto door = ed::Snap (doc, ctx, At (7.03, 6.98));
    EXPECT_EQ (door.kind, ed::SnapKind::Endpoint);
    EXPECT_EQ (door.contextSource, "Door threshold");
    EXPECT_NE (door.hint.find ("Door threshold"), std::string::npos);
    EXPECT_DOUBLE_EQ (door.point.z, 0.0); // kept on the working plane
    auto q = At (7.03, 6.98);
    q.keepPlane = false;
    EXPECT_DOUBLE_EQ (ed::Snap (doc, ctx, q).point.z, 1.2);
    EXPECT_EQ (ed::Snap (doc, ctx, At (5.02, 10.01)).kind, ed::SnapKind::Midpoint);
}

TEST (EditSnap, GridIsTheLastResort)
{
    ed::Document doc;
    auto q = At (1.26, 2.71);
    q.grid = 0.5;
    const auto r = ed::Snap (doc, {}, q);
    EXPECT_EQ (r.kind, ed::SnapKind::Grid);
    ExpectAt (r, 1.5, 2.5);
}

TEST (EditSnap, TerrainGivesHeightButNotPosition)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    ed::SnapContext ctx;
    ctx.height = [] (double x, double, double& z) {
        z = 10.0 + 0.1 * x;
        return x >= 0.0;
    };
    const auto free = ed::Snap (doc, ctx, At (2.0, 3.0));
    EXPECT_EQ (free.kind, ed::SnapKind::Terrain);
    ExpectAt (free, 2, 3);
    EXPECT_NEAR (free.point.z, 10.2, 1e-12);
    const auto end = ed::Snap (doc, ctx, At (4.03, 0.02));
    EXPECT_EQ (end.kind, ed::SnapKind::Endpoint);
    EXPECT_NEAR (end.point.z, 10.4, 1e-12);
    EXPECT_FALSE (ed::Snap (doc, ctx, At (-2.0, 3.0)).snapped); // off the terrain
}

TEST (EditSnap, VerticalWorkingPlaneSnapsToATopCorner)
{
    ed::Document doc;
    ed::AddExtrusion (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 3, 0 }, { 0, 3, 0 } }, 3.0);
    auto q = At (0, 0);
    q.cursor = { 3.97, 0.0, 3.04 };
    q.plane.normal = { 0, -1, 0 }; // front elevation
    q.keepPlane = false;
    const auto r = ed::Snap (doc, {}, q);
    EXPECT_EQ (r.kind, ed::SnapKind::Endpoint);
    EXPECT_NEAR (r.point.x, 4.0, 1e-12);
    EXPECT_NEAR (r.point.z, 3.0, 1e-12);
}

TEST (EditSnap, ParallelAndSquareToATouchedEdge)
{
    ed::Document doc;
    ed::SnapContext ctx;
    ed::SnapQuery q = At (3.0, 3.1, 0.2);
    q.anchor = ed::Vec3 { 0, 0, 0 };
    q.modes = ed::kSnapParallel;
    q.references = { { { 10, 0, 0 }, { 14, 4, 0 } } }; // an edge at 45 degrees, far away
    auto r = ed::Snap (doc, ctx, q);
    ASSERT_EQ (r.kind, ed::SnapKind::Parallel);
    ExpectAt (r, 3.05, 3.05);
    ASSERT_EQ (r.guides.size (), 2u); // the line through the anchor and the edge it follows
    EXPECT_NEAR (r.guides[1].from.x, 10.0, 1e-12);
    q.cursor = { -3.0, 3.1, 0 }; // square to it
    r = ed::Snap (doc, ctx, q);
    ASSERT_EQ (r.kind, ed::SnapKind::Parallel);
    ExpectAt (r, -3.05, 3.05);
    EXPECT_EQ (r.hint, "Square to edge");
}

TEST (EditSnap, ConstructionAnglesThroughTheAnchor)
{
    ed::Document doc;
    ed::SnapContext ctx;
    ed::SnapQuery q = At (5.0, 2.95, 0.1);
    q.anchor = ed::Vec3 { 0, 0, 0 };
    q.modes = ed::kSnapAngle;
    q.angleStep = 30.0;
    const auto r = ed::Snap (doc, ctx, q);
    ASSERT_EQ (r.kind, ed::SnapKind::Angle);
    EXPECT_NEAR (std::atan2 (r.point.y, r.point.x) * 180.0 / std::acos (-1.0), 30.0, 1e-9);
    EXPECT_EQ (r.hint.substr (0, 2), "30");
    q.angleStep = 0.0;
    EXPECT_FALSE (ed::Snap (doc, ctx, q).snapped);
}

TEST (EditSnap, EdgeSnapsReportTheirEdge)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    ed::SnapContext ctx;
    const auto mid = ed::Snap (doc, ctx, At (2.03, 0.02));
    ASSERT_EQ (mid.kind, ed::SnapKind::Midpoint);
    ASSERT_TRUE (mid.onEdge);
    EXPECT_NEAR (mid.edgeTo.x, 4.0, 1e-12);
}

TEST (EditSnap, MovedEdgesSnapFlushToParallelEdges)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false); // a wall line along y = 0
    ed::SnapContext ctx;
    ed::SnapQuery q = At (0, 0, 0.3);
    // A box's bottom edge, moved to y = 0.2 and a little along: it closes the gap.
    ed::Vec3 translate { 1.0, 0.2, 0.0 };
    ed::SnapResult r;
    ASSERT_TRUE (ed::SnapEdges (doc, ctx, q, { { { 2, 0.2, 0 }, { 5, 0.2, 0 } } }, translate, r));
    EXPECT_NEAR (translate.y, 0.0, 1e-12);
    EXPECT_NEAR (translate.x, 1.0, 1e-12); // along the edge, the move is the user's
    EXPECT_EQ (r.hint, "Edge to edge");
    // Out of reach, or not parallel: no snap.
    translate = { 0, 1.0, 0 };
    EXPECT_FALSE (ed::SnapEdges (doc, ctx, q, { { { 2, 1.0, 0 }, { 5, 1.0, 0 } } }, translate, r));
    translate = { 0, 0.1, 0 };
    EXPECT_FALSE (ed::SnapEdges (doc, ctx, q, { { { 2, 0.1, 0 }, { 5, 1.1, 0 } } }, translate, r));
}
