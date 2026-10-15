#include "Edit/EditGizmo.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditShape.hpp"
#include "Edit/EditTracker.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
// A 10 x 6 rectangle, counter-clockwise from the origin.
struct Box {
    ed::Document doc;
    ed::Id e = 0;
    Box ()
    {
        e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
    }
    const ed::Entity& E () const
    {
        return *ed::Find (doc, e);
    }
    ed::Id V (size_t i) const
    {
        return E ().vertices[i].id;
    }
};
} // namespace

TEST (EditShape, LiveCornerIsAnArcTangentToBothLegs)
{
    Box b;
    std::string error;
    ASSERT_TRUE (ed::SetCornerRadius (b.doc, b.e, { b.V (1) }, 2.0, error)) << error;
    EXPECT_DOUBLE_EQ (b.E ().vertices[1].radius, 2.0);
    EXPECT_EQ (b.E ().vertices.size (), 4u); // still four anchors: the corner is a radius, not points
    const auto shape = ed::Shape (b.E ());
    ASSERT_GT (shape.size (), 6u);
    // The arc runs from (8, 0) to (10, 2) about (8, 2), every point 2 m from it.
    bool t1 = false, t2 = false;
    for (const auto& p : shape) {
        t1 = t1 || (std::abs (p.x - 8) < 1e-9 && std::abs (p.y) < 1e-9);
        t2 = t2 || (std::abs (p.x - 10) < 1e-9 && std::abs (p.y - 2) < 1e-9);
        if (p.x > 8 + 1e-9 && p.y < 2 - 1e-9)
            EXPECT_NEAR (std::hypot (p.x - 8, p.y - 2), 2.0, 1e-9);
    }
    EXPECT_TRUE (t1);
    EXPECT_TRUE (t2);
    // Its grip sits at the arc's middle, on the bisector.
    const ed::Vec3 grip = ed::CornerGripAt (b.E (), 1, 0.1);
    EXPECT_NEAR (std::hypot (grip.x - 8, grip.y - 2), 2.0, 1e-9);
    EXPECT_NEAR (ed::RadiusFromGrip (b.E (), 1, grip, 0.1), 2.0, 1e-9);
}

TEST (EditShape, RadiusMustFitItsLegs)
{
    Box b;
    std::string error;
    EXPECT_NEAR (ed::MaxRadius (b.E (), { 1 }), 6.0, 1e-9);          // the short leg, alone
    EXPECT_NEAR (ed::MaxRadius (b.E (), { 0, 1, 2, 3 }), 3.0, 1e-9); // all four share the 6 m sides
    EXPECT_FALSE (ed::SetCornerRadius (b.doc, b.e, { b.V (1) }, 6.5, error));
    EXPECT_NE (error.find ("at most 6.000"), std::string::npos) << error;
    ASSERT_TRUE (ed::SetCornerRadius (b.doc, b.e, { b.V (2) }, 4.0, error)) << error;
    EXPECT_NEAR (ed::MaxRadius (b.E (), { 1 }), 2.0, 1e-9); // the neighbour took 4 of the 6
    // Moving a vertex can leave a leg too short: both corners give way in proportion.
    ASSERT_TRUE (ed::SetCornerRadius (b.doc, b.e, { b.V (1) }, 2.0, error)) << error;
    ed::Find (b.doc, b.e)->vertices[2].p.y = 3.0;
    ed::CornerGeometry c1, c2;
    ASSERT_TRUE (ed::CornerOf (b.E (), 1, c1) && ed::CornerOf (b.E (), 2, c2));
    EXPECT_NEAR (ed::TangentLength (c1, ed::DrawnRadius (b.E (), 1)) +
                     ed::TangentLength (c2, ed::DrawnRadius (b.E (), 2)),
                 3.0, 1e-9); // together they use the 3 m leg, no more
    EXPECT_FALSE (ed::SetCornerRadius (b.doc, b.e, { b.E ().vertices.size () > 0 ? b.V (0) : 0 }, -1.0, error));
}

TEST (EditShape, OpenEndsAreNoCorners)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 5, 0, 0 }, { 5, 5, 0 } }, false);
    std::string error;
    EXPECT_FALSE (ed::SetCornerRadius (doc, e, { ed::Find (doc, e)->vertices[0].id }, 1.0, error));
    EXPECT_TRUE (ed::SetCornerRadius (doc, e, { ed::Find (doc, e)->vertices[1].id }, 1.0, error)) << error;
}

TEST (EditShape, RadiusIsATypedDimensionAndFilletStaysLive)
{
    Box b;
    const ed::Ref corner { b.e, ed::Part::Vertex, b.V (1) };
    bool radius = false;
    for (const auto& d : ed::DimensionsFor (b.doc, corner))
        radius = radius || d.quantity == ed::Quantity::Radius;
    EXPECT_TRUE (radius);
    std::string error;
    ASSERT_TRUE (ed::WriteDimension ({ corner, ed::Quantity::Radius }, 1.5) (b.doc, error)) << error;
    double v = 0;
    ASSERT_TRUE (ed::ReadDimension (b.doc, { corner, ed::Quantity::Radius }, v));
    EXPECT_DOUBLE_EQ (v, 1.5);

    // The Round corner action keeps the radius; baking turns it into points.
    ed::Session s (b.doc);
    ed::Selection sel;
    sel.Set ({ { b.e, ed::Part::Vertex, b.V (3) } });
    const auto table = ed::PolylineActions ();
    for (const auto* a : ed::Applicable (table, sel, ed::ViewKind::Plan))
        if (a->id == "fillet")
            ASSERT_TRUE (s.Apply (a->make (s.Doc (), sel.Items ()))) << s.Message ();
    EXPECT_DOUBLE_EQ (ed::Find (s.Doc (), b.e)->vertices[3].radius, 0.5);
    EXPECT_EQ (ed::Find (s.Doc (), b.e)->vertices.size (), 4u);
    sel.Set ({ { b.e, ed::Part::Entity, 0 } });
    bool baked = false;
    for (const auto* a : ed::Applicable (table, sel, ed::ViewKind::Plan))
        if (a->id == "bake-corners") {
            ASSERT_TRUE (s.Apply (a->make (s.Doc (), sel.Items ()))) << s.Message ();
            baked = true;
        }
    EXPECT_TRUE (baked);
    const auto& e = *ed::Find (s.Doc (), b.e);
    EXPECT_GT (e.vertices.size (), 10u);
    for (const auto& vx : e.vertices)
        EXPECT_DOUBLE_EQ (vx.radius, 0.0);
}

namespace {
// A 10 m line bent into a curve by handles at both ends.
struct Curve {
    ed::Document doc;
    ed::Id e = 0;
    Curve ()
    {
        e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
        auto& v = ed::Find (doc, e)->vertices;
        v[0].out = { 3, 4, 0 };
        v[1].in = { -3, 4, 0 };
    }
    const ed::Entity& E () const
    {
        return *ed::Find (doc, e);
    }
};
} // namespace

TEST (EditShape, HandlesMakeASegmentACubic)
{
    Curve c;
    ASSERT_TRUE (ed::SegmentCurved (c.E (), 0));
    const ed::Vec3 mid = ed::SegmentPoint (c.E (), 0, 0.5);
    EXPECT_NEAR (mid.x, 5.0, 1e-12);
    EXPECT_NEAR (mid.y, 3.0, 1e-12); // (0 + 3*4 + 3*4 + 0) / 8
    const auto shape = ed::Shape (c.E ());
    EXPECT_EQ (shape.size (), 25u); // both ends and the curve between
    EXPECT_NEAR (ed::NearestOnSegment (c.E (), 0, { 5, 10, 0 }), 0.5, 1e-6);
}

TEST (EditShape, InsertingOnACurveKeepsItsShape)
{
    Curve c;
    const ed::Vec3 before = ed::SegmentPoint (c.E (), 0, 0.25);
    std::string error;
    ASSERT_TRUE (ed::InsertVertex (c.doc, c.e, c.E ().segments[0].id, ed::SegmentPoint (c.E (), 0, 0.5), error))
        << error;
    ASSERT_EQ (c.E ().vertices.size (), 3u);
    EXPECT_TRUE (c.E ().vertices[1].smooth);
    // The old t = 0.25 is the first half's t = 0.5.
    const ed::Vec3 after = ed::SegmentPoint (c.E (), 0, 0.5);
    EXPECT_NEAR (after.x, before.x, 1e-9);
    EXPECT_NEAR (after.y, before.y, 1e-9);
}

TEST (EditShape, SmoothHandlesStayInLine)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 6, 0, 0 }, { 12, 3, 0 } }, false);
    const ed::Id mid = ed::Find (doc, e)->vertices[1].id;
    std::string error;
    ASSERT_TRUE (ed::SetSmooth (doc, e, mid, true, error)) << error;
    const auto& v = ed::Find (doc, e)->vertices[1];
    EXPECT_TRUE (v.smooth);
    EXPECT_NEAR (std::hypot (v.in.x, v.in.y), 2.0, 1e-9); // a third of the way back
    // Moving one handle turns the other, keeping its length.
    ASSERT_TRUE (ed::SetHandle (doc, e, mid, true, { 0, 4, 0 }, false, error)) << error;
    EXPECT_NEAR (ed::Find (doc, e)->vertices[1].in.x, 0.0, 1e-12);
    EXPECT_NEAR (ed::Find (doc, e)->vertices[1].in.y, -2.0, 1e-12);
    // Independent: the other stays and the point becomes a corner.
    ASSERT_TRUE (ed::SetHandle (doc, e, mid, true, { 4, 0, 0 }, true, error)) << error;
    EXPECT_FALSE (ed::Find (doc, e)->vertices[1].smooth);
    EXPECT_NEAR (ed::Find (doc, e)->vertices[1].in.y, -2.0, 1e-12);
    ASSERT_TRUE (ed::SetSmooth (doc, e, mid, false, error)) << error;
    EXPECT_FALSE (ed::SegmentCurved (*ed::Find (doc, e), 0));
}

TEST (EditShape, BendingASegmentMovesItsPointWithThePointer)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
    std::string error;
    ASSERT_TRUE (ed::BendSegment (doc, e, ed::Find (doc, e)->segments[0].id, 0.5, { 0, 3, 0 }, error)) << error;
    const ed::Vec3 mid = ed::SegmentPoint (*ed::Find (doc, e), 0, 0.5);
    EXPECT_NEAR (mid.x, 5.0, 1e-9);
    EXPECT_NEAR (mid.y, 3.0, 1e-9);
    ASSERT_TRUE (ed::SetSegmentCurved (doc, e, ed::Find (doc, e)->segments[0].id, false, error));
    EXPECT_FALSE (ed::SegmentCurved (*ed::Find (doc, e), 0));
}

TEST (EditShape, CurvesTurnWithTheGizmoAndEndRoundings)
{
    Curve c;
    ed::Session s (c.doc);
    ed::Transform turn;
    turn.angle = std::acos (-1.0) / 2.0;
    ASSERT_TRUE (s.Apply (ed::TransformOperation ({ { c.e, ed::Part::Entity, 0 } }, turn))) << s.Message ();
    const auto& v = ed::Find (s.Doc (), c.e)->vertices;
    EXPECT_NEAR (v[0].out.x, -4.0, 1e-9); // (3, 4) turned a quarter
    EXPECT_NEAR (v[0].out.y, 3.0, 1e-9);
    // A corner next to a curved leg cannot be rounded.
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 5, 0, 0 }, { 5, 5, 0 } }, false);
    ed::Find (doc, e)->vertices[0].out = { 1, 1, 0 };
    std::string error;
    EXPECT_FALSE (ed::SetCornerRadius (doc, e, { ed::Find (doc, e)->vertices[1].id }, 0.5, error));
}

TEST (EditShape, ACurvedSegmentMeasuresAlongItsCurve)
{
    Curve c;
    const ed::Ref seg { c.e, ed::Part::Segment, c.E ().segments[0].id };
    const auto dims = ed::DimensionsFor (c.doc, seg);
    ASSERT_EQ (dims.size (), 1u); // its length; direction and fall are the handles'
    double length = 0;
    ASSERT_TRUE (ed::ReadDimension (c.doc, dims[0], length));
    EXPECT_GT (length, 11.0); // longer than its 10 m chord
    EXPECT_LT (length, 14.0);
    std::string error;
    EXPECT_FALSE (ed::WriteDimension (dims[0], 12.0) (c.doc, error));
    EXPECT_NE (error.find ("handles"), std::string::npos);
}
