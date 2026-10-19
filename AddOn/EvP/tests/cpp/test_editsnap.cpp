#include "Edit/EditShape.hpp"
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

TEST (EditSnap, ARoundedCornerIsSnappedAsAnArc)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, false);
    ed::Find (doc, e)->vertices[1].radius = 4.0; // arc from (6, 0) to (10, 4) about (6, 4)
    ed::SnapContext ctx;
    // On the arc: an edge snap, never a middle of one of its drawn pieces or a crossing of two.
    const double a = 3.14159265358979323846 * 1.6; // a point on the arc
    const ed::Vec3 on { 6 + 4 * std::cos (a), 4 + 4 * std::sin (a), 0 };
    auto r = ed::Snap (doc, ctx, At (on.x + 0.02, on.y - 0.01, 0.1));
    ASSERT_TRUE (r.snapped);
    EXPECT_EQ (r.kind, ed::SnapKind::Nearest);
    ASSERT_TRUE (r.onEdge);
    // The edge it reports is the arc's tangent there: square to the radius.
    const ed::Vec3 d = r.edgeTo - r.edgeFrom, radial = r.point - ed::Vec3 { 6, 4, 0 };
    EXPECT_NEAR ((d.x * radial.x + d.y * radial.y) / (std::hypot (d.x, d.y) * std::hypot (radial.x, radial.y)), 0.0,
                 0.08); // a drawn piece's direction: within half its 0.15 rad sweep of the true tangent
    // The arc's centre and its tangent points are points to snap to; the anchors' hidden
    // chord past the tangent point is not an edge.
    EXPECT_EQ (ed::Snap (doc, ctx, At (6.03, 4.02)).kind, ed::SnapKind::Centre);
    const auto t = ed::Snap (doc, ctx, At (6.02, 0.03));
    EXPECT_EQ (t.kind, ed::SnapKind::Endpoint);
    ExpectAt (t, 6.0, 0.0);
    EXPECT_FALSE (ed::Snap (doc, ctx, At (9.0, 0.02)).snapped); // the corner's chord, not drawn
}

TEST (EditSnap, SampledBendsFormRuns)
{
    std::vector<ed::Vec3> pts { { 0, 0, 0 }, { 5, 0, 0 } };
    for (int k = 1; k <= 8; ++k) {
        const double a = -3.14159265358979323846 / 2 + 3.14159265358979323846 / 2 * k / 8;
        pts.push_back ({ 5 + 3 * std::cos (a), 3 + 3 * std::sin (a), 0 });
    }
    pts.push_back ({ 8, 10, 0 });
    const auto runs = ed::CurveRuns (pts, false);
    ASSERT_EQ (runs.size (), pts.size () - 1);
    EXPECT_EQ (runs.front (), -1); // the straight lead-in
    EXPECT_EQ (runs[3], 0);        // inside the bend
    EXPECT_EQ (runs[5], 0);
}

// --- View screening (SnapQuery::screen): what the editor's Map supplies so a snap is mouse-close,
// on screen and near in 3D. Unset, every rule below is skipped (the tests above cover that path).

TEST (EditSnap, ViewScreeningRejectsABehindCameraCandidate)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 3, 0, 0 }, { 3, 4, 0 } }, false);
    ed::SnapQuery q = At (3.01, 0.01, 0.1);
    q.modes = ed::kSnapEndpoint;
    EXPECT_EQ (ed::Snap (doc, {}, q).kind, ed::SnapKind::Endpoint); // the plane metric alone sees it
    ed::SnapQuery::Screening s;
    s.project = [] (const ed::Vec3&, double&, double&) { return false; }; // behind the camera
    s.viewWidth = 1200.0, s.viewHeight = 800.0;
    q.screen = s;
    EXPECT_FALSE (ed::Snap (doc, {}, q).snapped);
}

TEST (EditSnap, ViewScreeningRejectsAFarCandidateBeforeAnyProjection)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 3, 0, 50 }, { 3, 4, 50 } }, false); // far above the working plane
    ed::SnapQuery q = At (3.01, 0.01, 0.1);
    q.modes = ed::kSnapEndpoint;
    EXPECT_EQ (ed::Snap (doc, {}, q).kind, ed::SnapKind::Endpoint);
    ed::SnapQuery::Screening s;
    // It would land exactly on the pointer, so only the world point can tell it is far away.
    s.project = [] (const ed::Vec3&, double& px, double& py) {
        px = 110.0, py = 100.0;
        return true;
    };
    s.mouseX = 110.0, s.mouseY = 100.0;
    s.pixelTolerance = 10.0;
    s.viewWidth = 1200.0, s.viewHeight = 800.0;
    s.proximity = 1.0; // metres
    q.screen = s;
    EXPECT_FALSE (ed::Snap (doc, {}, q).snapped);
}

TEST (EditSnap, ViewScreeningRejectsAnOffViewportCandidate)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 3, 0, 0 }, { 3, 4, 0 } }, false);
    ed::SnapQuery q = At (3.01, 0.01, 0.1);
    q.modes = ed::kSnapEndpoint;
    ed::SnapQuery::Screening s;
    s.project = [] (const ed::Vec3&, double& px, double& py) {
        px = 2000.0, py = 100.0;
        return true;
    };
    s.viewWidth = 1200.0, s.viewHeight = 800.0;
    q.screen = s;
    EXPECT_FALSE (ed::Snap (doc, {}, q).snapped);
}

TEST (EditSnap, ViewScreeningAcceptsANearOnScreenCandidateAndDropsAFarOne)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 3, 0, 0 }, { 3, 4, 0 } }, false);
    ed::SnapQuery q = At (3.01, 0.01, 0.1);
    q.modes = ed::kSnapEndpoint;
    ed::SnapQuery::Screening s;
    s.project = [] (const ed::Vec3& p, double& px, double& py) {
        px = 600.0 + 100.0 * p.x, py = 400.0;
        return true;
    };
    s.mouseX = 903.0, s.mouseY = 400.0; // the pointer where the endpoint shows
    s.pixelTolerance = 10.0;
    s.viewWidth = 1200.0, s.viewHeight = 800.0;
    q.screen = s;
    const auto near = ed::Snap (doc, {}, q);
    ASSERT_TRUE (near.snapped);
    EXPECT_EQ (near.kind, ed::SnapKind::Endpoint);
    s.mouseX = 943.0; // now 43 px away
    q.screen = s;
    EXPECT_FALSE (ed::Snap (doc, {}, q).snapped);
}

TEST (EditSnap, ViewScreeningKeepsASegmentCrossingTheViewport)
{
    ed::Document doc;
    ed::SnapContext ctx;
    ctx.segments.push_back ({ { -1000, 0, 0 }, { 1000, 0, 0 }, "model" }); // both ends far off screen
    ed::SnapQuery q = At (0.01, 0.0, 0.1);
    q.modes = ed::kSnapNearest;
    ed::SnapQuery::Screening s;
    s.project = [] (const ed::Vec3& p, double& px, double& py) {
        px = 600.0 + p.x, py = 400.0;
        return true;
    };
    s.mouseX = 600.0, s.mouseY = 400.0;
    s.pixelTolerance = 10.0;
    s.viewWidth = 1200.0, s.viewHeight = 800.0;
    s.proximity = 1.0;
    q.screen = s;
    const auto crossing = ed::Snap (doc, ctx, q);
    ASSERT_TRUE (crossing.snapped);
    EXPECT_EQ (crossing.kind, ed::SnapKind::Nearest);
    ExpectAt (crossing, 0.01, 0.0);
    // A lone endpoint off the viewport is not a candidate, though the crossing segment is.
    ed::SnapContext pointCtx;
    pointCtx.points.push_back ({ { 1000, 0, 0 }, "model" });
    ed::SnapQuery pq = At (1000.01, 0.0, 0.1);
    pq.modes = ed::kSnapEndpoint;
    pq.screen = s; // its pixel is 1600, past the 1200-wide viewport
    EXPECT_FALSE (ed::Snap (doc, pointCtx, pq).snapped);
}

TEST (EditSnap, EdgeFlushScreensTheTargetAndTakesTheGloballyNearestGrabbedEdge)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { -5, 0, 0 }, { 105, 0, 0 } }, false); // a long target line at y = 0
    ed::SnapContext ctx;
    ed::SnapQuery q = At (2.0, 0.05, 0.2);
    const std::vector<std::pair<ed::Vec3, ed::Vec3>> edges {
        { { 100, 0.05, 0 }, { 104, 0.05, 0 } }, // far from the pointer: gap 0.05
        { { 0, 0.1, 0 }, { 4, 0.1, 0 } },       // the grabbed edge: gap 0.1
    };
    ed::SnapResult r;
    // Unscreened, the first edge's smaller gap wins (the old first-edge behaviour).
    ed::Vec3 old { 0, 0, 0 };
    ASSERT_TRUE (ed::SnapEdges (doc, ctx, q, edges, old, r));
    EXPECT_NEAR (old.y, -0.05, 1e-9);
    // Screened, the far edge is not pointer-local, so the grabbed edge's gap is used instead.
    ed::SnapQuery::Screening s;
    s.proximity = 1.0;
    q.screen = s;
    ed::Vec3 now { 0, 0, 0 };
    ASSERT_TRUE (ed::SnapEdges (doc, ctx, q, edges, now, r));
    EXPECT_NEAR (now.y, -0.1, 1e-9);
}

TEST (EditSnap, ASegmentExclusionDropsItsWholeMovingItem)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 } }, false);
    const ed::Ref seg { e, ed::Part::Segment, ed::Find (doc, e)->segments[0].id };
    ed::SnapQuery q = At (4.02, 3.98, 0.2); // right at the far vertex (4, 4)
    q.modes = ed::kSnapEndpoint | ed::kSnapNearest;
    q.exclude = { seg };
    // The whole moving item is dropped, so its own far vertex is not a target.
    EXPECT_FALSE (ed::Snap (doc, {}, q).snapped);
}

// The new flush contract: a remote moving edge is never pulled onto a target the pointer is on,
// even though the perpendicular gap is within tolerance.
TEST (EditSnap, EdgeFlushRejectsARemoteMovingEdgeBesideANearbyTarget)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { -5, 0, 0 }, { 15, 0, 0 } }, false); // a target line at y = 0
    ed::SnapContext ctx;
    ed::SnapQuery q = At (0.0, 0.1, 0.2); // the pointer sits on the target
    // The moving edge is parallel and within the gap (0.1 m), but its nearest point is 5 m away.
    const std::vector<std::pair<ed::Vec3, ed::Vec3>> edges { { { 5, 0.1, 0 }, { 9, 0.1, 0 } } };
    ed::Vec3 translate { 0, 0, 0 };
    ed::SnapResult r;
    // Unscreened the gap alone would flush (the old broad behaviour) ...
    EXPECT_TRUE (ed::SnapEdges (doc, ctx, q, edges, translate, r));
    // ... but the moving edge is remote from the pointer, so the screened query refuses it.
    ed::SnapQuery::Screening s;
    s.proximity = 1.0; // metres
    q.screen = s;
    ed::Vec3 screened { 0, 0, 0 };
    EXPECT_FALSE (ed::SnapEdges (doc, ctx, q, edges, screened, r));
}

TEST (EditSnap, AProjectedIntersectionScreensBothOriginalSources)
{
    ed::SnapContext context;
    context.segments = { { { -0.5, 0, 0 }, { 0.5, 0, 0 }, "near" }, { { 0, -0.5, 50 }, { 0, 0.5, 50 }, "far" } };
    auto query = At (0.01, 0.01, 0.2);
    query.modes = ed::kSnapIntersection;
    query.screen = ed::SnapQuery::Screening {};
    query.screen->proximity = 1;
    EXPECT_FALSE (ed::Snap ({}, context, query).snapped);
    context.segments[1].a.z = context.segments[1].b.z = 0;
    EXPECT_EQ (ed::Snap ({}, context, query).kind, ed::SnapKind::Intersection);
}

TEST (EditSnap, TerrainElevationIsScreenedAfterSamplingItsRealHeight)
{
    ed::SnapContext context;
    context.height = [] (double, double, double& z) {
        z = 50;
        return true;
    };
    auto query = At (0, 0, 0.2);
    query.modes = ed::kSnapTerrain;
    query.screen = ed::SnapQuery::Screening {};
    query.screen->proximity = 1;
    EXPECT_FALSE (ed::Snap ({}, context, query).snapped);
    context.height = [] (double, double, double& z) {
        z = 0.25;
        return true;
    };
    const auto near = ed::Snap ({}, context, query);
    EXPECT_EQ (near.kind, ed::SnapKind::Terrain);
    EXPECT_DOUBLE_EQ (near.point.z, 0.25);
}
