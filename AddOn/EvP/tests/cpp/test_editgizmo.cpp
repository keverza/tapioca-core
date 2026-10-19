#include "Edit/EditGizmo.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
// A camera above the scene looking straight down at plan position (x, y).
ed::Ray Down (double x, double y)
{
    return { { x, y, 50 }, { 0, 0, -1 } };
}

// A camera in front of the scene looking along +Y at elevation (x, z).
ed::Ray Front (double x, double z)
{
    return { { x, -50, z }, { 0, 1, 0 } };
}

ed::Handle Find (ed::GizmoMode mode, ed::HandleKind kind, int axis)
{
    for (const auto& h : ed::Handles (mode))
        if (h.kind == kind && h.axis == axis)
            return h;
    ADD_FAILURE () << "no such handle";
    return {};
}

ed::Document Square (ed::Id& e)
{
    ed::Document doc;
    e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, true);
    return doc;
}
} // namespace

TEST (EditGizmo, HandleTablesAreData)
{
    const auto move = ed::Handles (ed::GizmoMode::Move);
    ASSERT_EQ (move.size (), 7u);
    int focusable = 0;
    for (size_t i = 0; i < move.size (); ++i) {
        EXPECT_EQ (move[i].index, i);
        focusable += move[i].focusable ? 1 : 0;
    }
    EXPECT_EQ (focusable, 3); // the three axes take typed distances; planes and centre do not
    EXPECT_EQ (ed::Handles (ed::GizmoMode::Extrude).size (), 1u);
    EXPECT_EQ (ed::Handles (ed::GizmoMode::Rotate)[1].quantity, ed::Quantity::Angle);
}

TEST (EditGizmo, APointHasMoveControlsButNoScaleRotationOrStretch)
{
    ed::Id id = 0;
    const auto doc = Square (id);
    const std::vector<ed::Ref> point { { id, ed::Part::Vertex, doc.entities[0].vertices[0].id } };
    const auto handles = ed::Handles (ed::GizmoMode::Gumball, doc, point, {});
    const auto table = ed::Handles (ed::GizmoMode::Gumball);
    ASSERT_EQ (handles.size (), table.size ());
    for (size_t i = 0; i < handles.size (); ++i) {
        const auto& h = handles[i];
        EXPECT_EQ (h.index, table[i].index);
        EXPECT_EQ (h.kind, table[i].kind);
        EXPECT_EQ (h.movable, h.kind == ed::HandleKind::Centre || h.kind == ed::HandleKind::Axis ||
                                  h.kind == ed::HandleKind::Plane);
        if (!h.movable) {
            EXPECT_FALSE (h.focusable);
            EXPECT_FALSE (ed::FromValue ({}, h, 2).ok);
            EXPECT_FALSE (ed::Drag ({}, h, Down (1, 1), Down (2, 2)).ok);
        }
    }
}

TEST (EditGizmo, TwoDistinctPointsCanScaleAndTurnButNotAlongAnEmptyAxis)
{
    ed::Id id = 0;
    const auto doc = Square (id);
    const std::vector<ed::Ref> points { { id, ed::Part::Vertex, doc.entities[0].vertices[0].id },
                                        { id, ed::Part::Vertex, doc.entities[0].vertices[1].id } };
    for (const auto& h : ed::Handles (ed::GizmoMode::Gumball, doc, points, {})) {
        if (h.kind == ed::HandleKind::ScaleAxis)
            EXPECT_EQ (h.movable, h.axis == 0);
        if (h.kind == ed::HandleKind::ScaleUniform)
            EXPECT_TRUE (h.movable);
        if (h.kind == ed::HandleKind::Ring)
            EXPECT_EQ (h.movable, h.axis != 0);
    }
}

TEST (EditGizmo, PathControlsKeepPlanTransformsSeparateFromProfileAndWidth)
{
    ed::Document doc;
    const auto id = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 1 } }, 2);
    const std::vector<ed::Ref> whole { { id, ed::Part::Entity, 0 } };
    for (const auto& h : ed::Handles (ed::GizmoMode::Gumball, doc, whole, {})) {
        if (h.kind == ed::HandleKind::Side)
            EXPECT_FALSE (h.movable);
        if (h.kind == ed::HandleKind::Ring)
            EXPECT_EQ (h.movable, h.axis == 2);
        if (h.kind == ed::HandleKind::ScaleAxis)
            EXPECT_EQ (h.movable, h.axis != 2);
        if (h.kind == ed::HandleKind::Axis)
            EXPECT_TRUE (h.movable);
    }
    for (const auto& h : ed::Handles (ed::GizmoMode::Extrude, doc, whole, {}))
        EXPECT_FALSE (h.movable);
    const std::vector<ed::Ref> point { { id, ed::Part::PathPoint, doc.paths[0].points[1].id } };
    for (const auto& h : ed::Handles (ed::GizmoMode::Gumball, doc, point, {}))
        if (h.kind == ed::HandleKind::ScaleAxis || h.kind == ed::HandleKind::ScaleUniform ||
            h.kind == ed::HandleKind::Ring || h.kind == ed::HandleKind::Side)
            EXPECT_FALSE (h.movable);
    auto mixed = whole;
    mixed.push_back ({ id, ed::Part::Range, doc.paths[0].sections[0].id });
    for (const auto& h : ed::Handles (ed::GizmoMode::Gumball, doc, mixed, {}))
        EXPECT_FALSE (h.movable);
}

TEST (EditGizmo, AFaceOffersNormalPushPullWhileAnExtrusionVertexCannotTiltTheBase)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, 3);
    ed::Frame frame;
    ASSERT_TRUE (ed::FaceFrame (doc.entities[0], 1, frame));
    const std::vector<ed::Ref> top { { id, ed::Part::Face, 1 } };
    int movable = 0;
    for (const auto& h : ed::Handles (ed::GizmoMode::Gumball, doc, top, frame))
        if (h.movable) {
            ++movable;
            EXPECT_EQ (h.kind, ed::HandleKind::Face);
            const auto result = ed::FromValue (frame, h, 1.5);
            ASSERT_TRUE (result.ok);
            ed::Session session (doc);
            ASSERT_TRUE (session.Apply (ed::TransformOperation (top, result.transform)));
            EXPECT_DOUBLE_EQ (session.Doc ().entities[0].height, 4.5);
            EXPECT_EQ (session.Doc ().entities[0].vertices, doc.entities[0].vertices);
        }
    EXPECT_EQ (movable, 1);
    const std::vector<ed::Ref> vertex { { id, ed::Part::Vertex, doc.entities[0].vertices[0].id } };
    for (const auto& h : ed::Handles (ed::GizmoMode::Move, doc, vertex, {}))
        if (h.kind == ed::HandleKind::Axis)
            EXPECT_EQ (h.movable, h.axis != 2);
}

TEST (EditGizmo, AWholeExtrusionsBoxIsMeasuredToItsTopAndHasNoVerticalScale)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 } }, 3);
    const std::vector<ed::Ref> whole { { id, ed::Part::Entity, 0 } };
    // The box (and so the pivot every mode uses) is the shape's: mid-height, not its base.
    ed::Frame box;
    ASSERT_TRUE (ed::FitBox (doc, whole, box));
    EXPECT_NEAR (box.origin.z, 1.5, 1e-12);
    EXPECT_NEAR (box.half[2], 1.5, 1e-12);
    EXPECT_NEAR (box.half[0], 2.0, 1e-12);
    // No Z scale for an extrusion: its height is not a vertex extent, and the table says so
    // rather than depending on the vertices happening to share one height.
    doc.entities[0].vertices[0].p.z = 1.0; // a hand-made z extent, as a future box change would
    for (const auto& h : ed::Handles (ed::GizmoMode::Gumball, doc, whole, box))
        if (h.kind == ed::HandleKind::ScaleAxis)
            EXPECT_EQ (h.movable, h.axis != 2) << "an extrusion's height is not a scale axis";
}

TEST (EditGizmo, AUniformScaleOfAWholeExtrusionLeavesItsProfilePlanarAndItsHeightAlone)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 } }, 3);
    const std::vector<ed::Ref> whole { { id, ed::Part::Entity, 0 } };
    ed::Frame frame;
    ASSERT_TRUE (ed::FitBox (doc, whole, frame));
    ed::Transform t;
    t.pivot = frame.origin; // the box's centre: mid-height
    t.scaleFrame = frame;
    t.scale = { 2.0, 2.0, 2.0 };
    ed::Session session (doc);
    ASSERT_TRUE (session.Apply (ed::TransformOperation (whole, t)));
    EXPECT_DOUBLE_EQ (session.Doc ().entities[0].height, 3.0); // its own parameter, not scaled
    for (const auto& v : session.Doc ().entities[0].vertices)
        EXPECT_NEAR (v.p.z, 0.0, 1e-12);                                 // and the profile does not sink or tilt
    EXPECT_NEAR (session.Doc ().entities[0].vertices[2].p.x, 6.0, 1e-9); // the plan doubles about the box centre
}

TEST (EditGizmo, NeutralDragAsksTheControlWithoutAskingItsOpeningValue)
{
    ed::Frame f;
    // The Z axis seen from straight above points into the view: no drag can start there.
    EXPECT_FALSE (ed::NeutralDrag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Axis, 2), Down (1, 1)).ok);
    EXPECT_TRUE (ed::NeutralDrag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Axis, 2), Front (1, 1)).ok);
    // A side grip opens at the width it already has: a narrow selection is still draggable
    // (the first real move owns "that side would cross the other"), where a zero move is not.
    const auto grip = Find (ed::GizmoMode::Stretch, ed::HandleKind::Side, 0);
    ed::Frame narrow;
    narrow.half[0] = 1.0e-4;
    EXPECT_FALSE (ed::Drag (narrow, grip, Front (0, 0), Front (0, 0)).ok);
    EXPECT_TRUE (ed::NeutralDrag (narrow, grip, Front (0, 0)).ok);
}

TEST (EditGizmo, AxisDragFollowsTheRayNotTheScreen)
{
    ed::Frame f;
    const auto r = ed::Drag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Axis, 0), Down (1, 0.3), Down (3.5, 0.9));
    ASSERT_TRUE (r.ok) << r.error;
    EXPECT_NEAR (r.value, 2.5, 1e-12);
    EXPECT_NEAR (r.transform.translate.x, 2.5, 1e-12);
    EXPECT_NEAR (r.transform.translate.y, 0.0, 1e-12);
}

TEST (EditGizmo, EdgeOnHandlesAreRefused)
{
    ed::Frame f;
    // The Z axis seen from directly above points into the view.
    EXPECT_FALSE (ed::Drag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Axis, 2), Down (0, 0), Down (1, 1)).ok);
    // The XY plane seen from the front is a line.
    EXPECT_FALSE (ed::Drag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Plane, 2), Front (0, 0), Front (1, 0)).ok);
    // ...but Z from the front is fine.
    const auto z = ed::Drag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Axis, 2), Front (0, 1), Front (0, 2.25));
    ASSERT_TRUE (z.ok);
    EXPECT_NEAR (z.value, 1.25, 1e-12);
}

TEST (EditGizmo, PlaneDragAndCentreRelocation)
{
    ed::Frame f;
    const auto p = ed::Drag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Plane, 2), Down (1, 1), Down (4, -1));
    ASSERT_TRUE (p.ok);
    EXPECT_NEAR (p.transform.translate.x, 3.0, 1e-12);
    EXPECT_NEAR (p.transform.translate.y, -2.0, 1e-12);
    const auto c = ed::Drag (f, Find (ed::GizmoMode::Move, ed::HandleKind::Centre, -1), Down (0, 0), Down (1, 0));
    ASSERT_TRUE (c.ok);
    EXPECT_TRUE (c.transform.relocate);
}

TEST (EditGizmo, RingTurnsAboutTheFrameAxis)
{
    ed::Frame f;
    f.origin = { 1, 1, 0 };
    const auto r = ed::Drag (f, Find (ed::GizmoMode::Rotate, ed::HandleKind::Ring, 2), Down (2, 1), Down (1, 2));
    ASSERT_TRUE (r.ok);
    EXPECT_NEAR (r.value, 90.0, 1e-9);
    const ed::Vec3 p = ed::Apply (r.transform, { 3, 1, 0 });
    EXPECT_NEAR (p.x, 1.0, 1e-12);
    EXPECT_NEAR (p.y, 3.0, 1e-12);
}

TEST (EditGizmo, ScaleAlongAnAxisAndUniformly)
{
    ed::Frame f;
    const auto a = ed::Drag (f, Find (ed::GizmoMode::Scale, ed::HandleKind::ScaleAxis, 0), Down (2, 0), Down (3, 0));
    ASSERT_TRUE (a.ok);
    EXPECT_NEAR (a.value, 1.5, 1e-12);
    const ed::Vec3 p = ed::Apply (a.transform, { 2, 2, 0 });
    EXPECT_NEAR (p.x, 3.0, 1e-12);
    EXPECT_NEAR (p.y, 2.0, 1e-12);
    const auto u = ed::Drag (f, Find (ed::GizmoMode::Scale, ed::HandleKind::ScaleUniform, 2), Down (1, 0), Down (0, 2));
    ASSERT_TRUE (u.ok);
    EXPECT_NEAR (u.value, 2.0, 1e-12);
}

TEST (EditGizmo, TypedValuesMatchDrags)
{
    ed::Frame f;
    const auto h = Find (ed::GizmoMode::Move, ed::HandleKind::Axis, 1);
    const auto typed = ed::FromValue (f, h, 2.0);
    ASSERT_TRUE (typed.ok);
    const auto dragged = ed::Drag (f, h, Down (0, 1), Down (0, 3));
    EXPECT_NEAR (typed.transform.translate.y, dragged.transform.translate.y, 1e-12);
    EXPECT_FALSE (ed::FromValue (f, Find (ed::GizmoMode::Move, ed::HandleKind::Plane, 2), 1.0).ok);
    EXPECT_FALSE (ed::FromValue (f, Find (ed::GizmoMode::Scale, ed::HandleKind::ScaleAxis, 0), -1.0).ok);
}

TEST (EditGizmo, NavigationContinuationKeepsTheOriginalFrameAndTotalValue)
{
    ed::Frame frame;
    frame.half[0] = 2;
    for (const auto& h : ed::Handles (ed::GizmoMode::Gumball)) {
        const auto checkpoint = ed::Drag (frame, h, Down (2, 1), Down (3, 2));
        if (!checkpoint.ok)
            continue; // edge-on handles remain unsupported in this view
        const auto still = ed::ContinueDrag (frame, h, checkpoint, Down (8, 3), Down (8, 3));
        ASSERT_TRUE (still.ok) << still.error;
        EXPECT_NEAR (still.value, checkpoint.value, 1e-9);
        const ed::Vec3 a = ed::Apply (still.transform, { 4, 5, 0 });
        const ed::Vec3 b = ed::Apply (checkpoint.transform, { 4, 5, 0 });
        EXPECT_NEAR (ed::Distance (a, b), 0, 1e-9);
        EXPECT_NEAR (still.transform.stretchBy, checkpoint.transform.stretchBy, 1e-9);
    }
    const auto scale = Find (ed::GizmoMode::Scale, ed::HandleKind::ScaleAxis, 0);
    const auto r = ed::ContinueDrag (frame, scale, ed::FromValue (frame, scale, 2), Down (8, 0), Down (12, 0));
    ASSERT_TRUE (r.ok);
    EXPECT_NEAR (r.value, 3, 1e-9); // factors multiply, distances and angles add
    const auto ring = Find (ed::GizmoMode::Rotate, ed::HandleKind::Ring, 2);
    const auto turn = ed::ContinueDrag (frame, ring, ed::FromValue (frame, ring, 45), Down (8, 0), Down (0, 8));
    ASSERT_TRUE (turn.ok);
    EXPECT_NEAR (turn.value, 135, 1e-9);
}

TEST (EditGizmo, MovingAWholeEntityOrStretchingOneSegment)
{
    ed::Id e = 0;
    ed::Session s (Square (e));
    ed::Transform t;
    t.translate = { 1, 0, 0 };
    ASSERT_TRUE (s.Apply (ed::TransformOperation ({ { e, ed::Part::Entity, 0 } }, t)));
    EXPECT_DOUBLE_EQ (ed::Find (s.Doc (), e)->vertices[0].p.x, 1.0);
    // Stretch: only the right edge moves; its neighbours follow because they share vertices.
    const ed::Entity& sq = *ed::Find (s.Doc (), e);
    const ed::Ref right { e, ed::Part::Segment, sq.segments[1].id };
    ASSERT_TRUE (s.Apply (ed::TransformOperation ({ right }, t)));
    const ed::Entity& out = *ed::Find (s.Doc (), e);
    EXPECT_DOUBLE_EQ (out.vertices[0].p.x, 1.0);
    EXPECT_DOUBLE_EQ (out.vertices[1].p.x, 4.0);
    EXPECT_DOUBLE_EQ (out.vertices[2].p.x, 4.0);
}

TEST (EditGizmo, LockedVerticesRefuseTheTransform)
{
    ed::Id e = 0;
    ed::Document doc = Square (e);
    ed::Find (doc, e)->vertices[0].lockZ = true;
    ed::Session s (doc);
    ed::Transform up;
    up.translate = { 0, 0, 1 };
    EXPECT_FALSE (s.Apply (ed::TransformOperation ({ { e, ed::Part::Entity, 0 } }, up)));
    EXPECT_NE (s.Message ().find ("locked"), std::string::npos);
    ed::Transform side;
    side.translate = { 1, 0, 0 }; // XY is free
    EXPECT_TRUE (s.Apply (ed::TransformOperation ({ { e, ed::Part::Entity, 0 } }, side)));
}

TEST (EditGizmo, PushPullFacesOfAnExtrusion)
{
    ed::Document doc;
    const ed::Id box = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 3, 0 }, { 0, 3, 0 } }, 3.0);
    ed::Session s (doc);
    ed::Frame top;
    ASSERT_TRUE (ed::FaceFrame (*ed::Find (s.Doc (), box), 1, top));
    const auto up = ed::Drag (top, ed::Handles (ed::GizmoMode::Extrude)[0], Front (2, 3), Front (2, 4.5));
    ASSERT_TRUE (up.ok) << up.error;
    ASSERT_TRUE (s.Apply (ed::TransformOperation ({ { box, ed::Part::Face, 1 } }, up.transform)));
    EXPECT_DOUBLE_EQ (ed::Find (s.Doc (), box)->height, 4.5);

    // Side face over (4,0)-(4,3) faces +X: pushing it out widens the box.
    ed::Transform push;
    push.extrude = 1.0;
    ASSERT_TRUE (s.Apply (ed::TransformOperation ({ { box, ed::Part::Face, 3 } }, push)));
    const ed::Entity& e = *ed::Find (s.Doc (), box);
    EXPECT_DOUBLE_EQ (e.vertices[1].p.x, 5.0);
    EXPECT_DOUBLE_EQ (e.vertices[2].p.x, 5.0);
    // Pulling the top below the base is refused by validation.
    ed::Transform crush;
    crush.extrude = -10.0;
    EXPECT_FALSE (s.Apply (ed::TransformOperation ({ { box, ed::Part::Face, 1 } }, crush)));
}

TEST (EditGizmo, ExtrusionsRefuseATiltingMove)
{
    ed::Document doc;
    const ed::Id box = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 3, 0 } }, 1.0);
    ed::Session s (doc);
    ed::Transform lift;
    lift.translate = { 0, 0, 1 };
    const ed::Id v = ed::Find (s.Doc (), box)->vertices[0].id;
    EXPECT_FALSE (s.Apply (ed::TransformOperation ({ { box, ed::Part::Vertex, v } }, lift)));
    EXPECT_NE (s.Message ().find ("not flat"), std::string::npos);
}

TEST (EditGizmo, SegmentFrameIsPathLocal)
{
    ed::Frame f;
    ASSERT_TRUE (ed::SegmentFrame ({ 0, 0, 5 }, { 0, 3, 6 }, f));
    EXPECT_NEAR (f.axes[0].y, 1.0, 1e-12);
    EXPECT_NEAR (f.axes[1].x, -1.0, 1e-12); // left of the direction of travel
    EXPECT_FALSE (ed::SegmentFrame ({ 0, 0, 0 }, { 0, 0, 3 }, f));
}

TEST (EditGizmo, HandlesHaveGeometryAndCanBePicked)
{
    ed::Frame f;
    f.origin = { 1, 1, 0 };
    const auto handles = ed::Handles (ed::GizmoMode::Move);
    const auto shapes = ed::HandleGeometry (f, handles[1], 2.0); // X axis
    ASSERT_EQ (shapes.size (), 1u);
    EXPECT_NEAR (shapes[0].points[1].x, 3.0, 1e-12);
    ed::PickView plan;
    plan.toScreen = [] (ed::Vec3 p, double& x, double& y) {
        x = p.x * 100.0;
        y = p.y * 100.0;
        return true;
    };
    EXPECT_EQ (ed::PickHandle (f, handles, 2.0, plan, 250, 102, 6), 1);  // on the X axis
    EXPECT_EQ (ed::PickHandle (f, handles, 2.0, plan, 102, 250, 6), 2);  // on the Y axis
    EXPECT_EQ (ed::PickHandle (f, handles, 2.0, plan, 100, 100, 6), 0);  // the centre
    EXPECT_EQ (ed::PickHandle (f, handles, 2.0, plan, 600, 600, 6), -1); // nothing
    const auto ring = ed::HandleGeometry (f, ed::Handles (ed::GizmoMode::Rotate)[3], 1.0);
    ASSERT_EQ (ring.size (), 1u);
    EXPECT_TRUE (ring[0].closed);
    EXPECT_NEAR (ed::Distance (ring[0].points[0], f.origin), 0.8, 1e-12);
}

TEST (EditGizmo, SelectionFrameTurnsWithTheShape)
{
    ed::Document doc;
    const double c = std::cos (0.5), s = std::sin (0.5); // a 4 x 2 rectangle turned 0.5 rad
    const ed::Id e = ed::AddPolyline (
        doc, { { 0, 0, 0 }, { 4 * c, 4 * s, 0 }, { 4 * c - 2 * s, 4 * s + 2 * c, 0 }, { -2 * s, 2 * c, 0 } }, true);
    const auto& entity = *ed::Find (doc, e);
    const ed::Frame f = ed::SelectionFrame (doc, { { e, ed::Part::Entity, 0 } }, { 1, 1, 0 });
    EXPECT_NEAR (f.axes[0].x, c, 1e-12); // along the long side
    EXPECT_NEAR (f.axes[0].y, s, 1e-12);
    EXPECT_NEAR (f.axes[2].z, 1.0, 1e-12);
    EXPECT_NEAR (f.origin.x, 1.0, 1e-12);
    const ed::Frame side = ed::SelectionFrame (doc, { { e, ed::Part::Segment, entity.segments[1].id } }, {});
    EXPECT_NEAR (side.axes[0].x, -s, 1e-12); // a segment's own direction
    EXPECT_NEAR (side.axes[0].y, c, 1e-12);
    // Two entities, or a path: the world's.
    const ed::Id other = ed::AddPolyline (doc, { { 9, 9, 0 }, { 10, 9, 0 } }, false);
    const ed::Frame both = ed::SelectionFrame (doc, { { e, ed::Part::Entity, 0 }, { other, ed::Part::Entity, 0 } }, {});
    EXPECT_NEAR (both.axes[0].x, 1.0, 1e-12);
}

TEST (EditGizmo, FrameRidesItsTransform)
{
    ed::Frame f;
    f.origin = { 1, 0, 0 };
    ed::Transform t;
    t.pivot = { 0, 0, 0 };
    t.angle = std::acos (-1.0) / 2.0;
    t.translate = { 0, 0, 2 };
    const ed::Frame m = ed::Moved (f, t);
    EXPECT_NEAR (m.origin.x, 0.0, 1e-12);
    EXPECT_NEAR (m.origin.y, 1.0, 1e-12);
    EXPECT_NEAR (m.origin.z, 2.0, 1e-12);
    EXPECT_NEAR (m.axes[0].y, 1.0, 1e-12); // X turned a quarter
    EXPECT_NEAR (m.axes[1].x, -1.0, 1e-12);
}

TEST (EditGizmo, StretchGripsMoveOneSideAndReadTheWidth)
{
    ed::Id e = 0;
    ed::Session s (Square (e)); // 2 x 2 at the origin
    const std::vector<ed::Ref> all { { e, ed::Part::Entity, 0 } };
    ed::Frame f;
    ASSERT_TRUE (ed::FitBox (s.Doc (), all, f));
    EXPECT_NEAR (f.origin.x, 1.0, 1e-12);
    EXPECT_NEAR (f.half[0], 1.0, 1e-12);
    const auto grips = ed::Handles (ed::GizmoMode::Stretch);
    ASSERT_EQ (grips.size (), 4u);
    const ed::Handle right = grips[0], left = grips[1];
    EXPECT_EQ (right.side, 1);
    EXPECT_EQ (left.side, -1);
    EXPECT_EQ (right.quantity, ed::Quantity::Width);

    // Dragging the right grip 1.5 m out: the right side moves, the left stays, width 3.5.
    const auto r = ed::Drag (f, right, Down (2.2, 1), Down (3.7, 1));
    ASSERT_TRUE (r.ok) << r.error;
    EXPECT_NEAR (r.value, 3.5, 1e-9);
    ASSERT_TRUE (s.Apply (ed::TransformOperation (all, r.transform)));
    const auto& v = ed::Find (s.Doc (), e)->vertices;
    EXPECT_NEAR (v[0].p.x, 0.0, 1e-9);
    EXPECT_NEAR (v[1].p.x, 3.5, 1e-9);
    EXPECT_NEAR (v[2].p.x, 3.5, 1e-9);
    EXPECT_NEAR (v[3].p.x, 0.0, 1e-9);

    // A typed width on the left grip: the right side stays.
    ASSERT_TRUE (ed::FitBox (s.Doc (), all, f));
    const auto typed = ed::FromValue (f, left, 5.0);
    ASSERT_TRUE (typed.ok) << typed.error;
    ASSERT_TRUE (s.Apply (ed::TransformOperation (all, typed.transform)));
    EXPECT_NEAR (ed::Find (s.Doc (), e)->vertices[0].p.x, -1.5, 1e-9);
    EXPECT_NEAR (ed::Find (s.Doc (), e)->vertices[1].p.x, 3.5, 1e-9);

    // Dragging a side past the other is refused, not mirrored.
    ASSERT_TRUE (ed::FitBox (s.Doc (), all, f));
    EXPECT_FALSE (ed::Drag (f, right, Down (3.5, 1), Down (-3.0, 1)).ok);
}

TEST (EditGizmo, StretchingASegmentSidewaysPushesTheWholeEdge)
{
    ed::Id e = 0;
    ed::Session s (Square (e));
    const ed::Entity& sq = *ed::Find (s.Doc (), e);
    const std::vector<ed::Ref> edge { { e, ed::Part::Segment, sq.segments[1].id } }; // x = 2, vertical
    ed::Frame f = ed::SelectionFrame (s.Doc (), edge, {});
    ASSERT_TRUE (ed::FitBox (s.Doc (), edge, f));
    // Axis 1 of a segment's frame is across it; this edge has no width that way.
    const auto grips = ed::Handles (ed::GizmoMode::Stretch);
    const auto r = ed::FromValue (f, grips[3], 0.5); // axis 1, side -1: a quarter right of travel
    ASSERT_TRUE (r.ok) << r.error;
    ASSERT_TRUE (s.Apply (ed::TransformOperation (edge, r.transform)));
    EXPECT_NEAR (ed::Find (s.Doc (), e)->vertices[1].p.x, 2.5, 1e-9);
    EXPECT_NEAR (ed::Find (s.Doc (), e)->vertices[2].p.x, 2.5, 1e-9);
    EXPECT_NEAR (ed::Find (s.Doc (), e)->vertices[0].p.x, 0.0, 1e-9);
}

TEST (EditGizmo, HandlesSeenEdgeOnAreNeitherDrawnNorPicked)
{
    EXPECT_TRUE (ed::FacesTheView ({ { 5, 5 } }, false));                                     // the centre
    EXPECT_TRUE (ed::FacesTheView ({ { 0, 0 }, { 40, 0 } }, false));                          // an axis across the view
    EXPECT_FALSE (ed::FacesTheView ({ { 0, 0 }, { 0.5, 0.5 } }, false));                      // an axis into the view
    EXPECT_TRUE (ed::FacesTheView ({ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } }, true));    // a plane face-on
    EXPECT_FALSE (ed::FacesTheView ({ { 0, 0 }, { 10, 0 }, { 10, 0.2 }, { 0, 0.2 } }, true)); // ...side-on
    // In plan the planes and rings about X and Y are side-on: a pick on their line finds nothing.
    ed::Frame f;
    ed::PickView plan;
    plan.toScreen = [] (ed::Vec3 p, double& x, double& y) {
        x = p.x * 100.0;
        y = p.y * 100.0;
        return true;
    };
    const auto rotate = ed::Handles (ed::GizmoMode::Rotate);
    EXPECT_EQ (ed::PickHandle (f, rotate, 1.0, plan, 0, 60, 4), -1) << "the X ring is a line in plan";
    EXPECT_EQ (ed::PickHandle (f, rotate, 1.0, plan, 80, 0, 4), 3) << "the Z ring faces the plan";
}
