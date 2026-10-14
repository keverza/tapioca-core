#include "Edit/EditGizmo.hpp"
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
