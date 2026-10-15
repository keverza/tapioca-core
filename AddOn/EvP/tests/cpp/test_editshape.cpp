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
