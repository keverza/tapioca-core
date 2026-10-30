#include "Edit/EditPrimitives.hpp"
#include "Edit/EditEllipse.hpp"
#include "Edit/EditGizmo.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditShape.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
double SignedArea (const std::vector<ed::Vec3>& p)
{
    double a = 0.0;
    for (size_t i = 0; i < p.size (); ++i) {
        const auto& q = p[i];
        const auto& r = p[(i + 1) % p.size ()];
        a += q.x * r.y - r.x * q.y;
    }
    return a * 0.5;
}

ed::PrimitiveInput Input (ed::Primitive kind, ed::Vec3 first, ed::Vec3 second)
{
    ed::PrimitiveInput in;
    in.kind = kind;
    in.first = first;
    in.second = second;
    return in;
}
} // namespace

TEST (EditPrimitives, ARectangleRunsCounterClockwiseWhicheverWayItIsDragged)
{
    for (const ed::Vec3 corner :
         { ed::Vec3 { 4, 2, 0 }, ed::Vec3 { -4, 2, 0 }, ed::Vec3 { -4, -2, 0 }, ed::Vec3 { 4, -2, 0 } }) {
        ed::PrimitiveOutline out;
        std::string error;
        ASSERT_TRUE (ed::OutlinePrimitive (Input (ed::Primitive::Rectangle, { 1, 1, 3 }, corner + ed::Vec3 { 1, 1, 3 }),
                                           out, error))
            << error;
        ASSERT_EQ (out.points.size (), 4u);
        EXPECT_NEAR (SignedArea (out.points), 8.0, 1e-12);
        for (const auto& p : out.points)
            EXPECT_EQ (p.z, 3.0) << "on the plane it was drawn on";
        EXPECT_TRUE (out.in.empty ());
    }
    auto square = Input (ed::Primitive::Rectangle, {}, { 3, 1, 0 });
    square.uniform = true;
    ed::PrimitiveOutline out;
    std::string error;
    ASSERT_TRUE (ed::OutlinePrimitive (square, out, error));
    EXPECT_NEAR (SignedArea (out.points), 9.0, 1e-12);
    EXPECT_FALSE (ed::OutlinePrimitive (Input (ed::Primitive::Rectangle, {}, { 3, 0, 0 }), out, error));
}

TEST (EditPrimitives, APolygonIsRegularAndTurnsWithItsSecondClick)
{
    auto in = Input (ed::Primitive::Polygon, { 5, 5, 0 }, { 5, 7, 0 });
    for (const int sides : { 3, 6, 17 }) {
        in.sides = sides;
        ed::PrimitiveOutline out;
        std::string error;
        ASSERT_TRUE (ed::OutlinePrimitive (in, out, error)) << error;
        ASSERT_EQ (out.points.size (), size_t (sides));
        EXPECT_NEAR (out.points[0].x, 5.0, 1e-12); // the first vertex is the second click
        EXPECT_NEAR (out.points[0].y, 7.0, 1e-12);
        for (const auto& p : out.points)
            EXPECT_NEAR (std::hypot (p.x - 5, p.y - 5), 2.0, 1e-12);
        EXPECT_GT (SignedArea (out.points), 0.0);
    }
    for (const int bad : { 2, ed::kMaxPolygonSides + 1 }) {
        in.sides = bad;
        ed::PrimitiveOutline out;
        std::string error;
        EXPECT_FALSE (ed::OutlinePrimitive (in, out, error));
    }
}

TEST (EditPrimitives, AnEllipsesQuarterCurvesPassThroughTheTrueEllipse)
{
    for (const bool circle : { false, true }) {
        auto in = Input (ed::Primitive::Ellipse, { 0, 0, 1 }, { 4, 2, 1 });
        in.uniform = circle;
        ed::Document doc;
        std::string error;
        const auto id = ed::AddPrimitive (doc, in, "test", error);
        ASSERT_NE (id, ed::kNoId) << error;
        const ed::Entity* e = ed::Find (doc, id);
        ASSERT_NE (e, nullptr);
        EXPECT_TRUE (e->closed);
        ASSERT_TRUE (e->ellipse.has_value ());
        EXPECT_EQ (e->ellipse->mode, circle ? ed::EllipseMode::Circle : ed::EllipseMode::Ellipse);
        ASSERT_EQ (e->vertices.size (), 4u);
        const double rx = circle ? std::hypot (4.0, 2.0) : 4.0, ry = circle ? rx : 2.0;
        for (size_t i = 0; i < e->segments.size (); ++i) {
            EXPECT_TRUE (e->vertices[i].smooth);
            const auto points = ed::SegmentPoints (*e, i);
            ASSERT_GE (points.size (), 3u);
            const auto& mid = points[points.size () / 2];
            // A cubic quarter is within 0.03 % of the ellipse; its midpoint exactly on it.
            for (const auto& p : points)
                EXPECT_NEAR (std::hypot (p.x / rx, p.y / ry), 1.0, 3e-4);
            EXPECT_NEAR (mid.z, 1.0, 1e-12);
        }
    }
}

TEST (EditEllipse, TypedParametersPreserveModeAndAreUndoable)
{
    ed::Document doc;
    auto ellipseInput = Input (ed::Primitive::Ellipse, { 10, 20, 3 }, { 14, 22, 3 });
    std::string error;
    const ed::Id ellipse = ed::AddPrimitive (doc, ellipseInput, "typed", error);
    ASSERT_NE (ellipse, ed::kNoId) << error;
    ed::EllipseParameters parameters;
    ASSERT_TRUE (ed::GetEllipseParameters (*ed::Find (doc, ellipse), parameters, error)) << error;
    EXPECT_EQ (parameters.mode, ed::EllipseMode::Ellipse);
    EXPECT_EQ (parameters.center, (ed::Vec3 { 10, 20, 3 }));
    EXPECT_DOUBLE_EQ (parameters.majorRadius, 4.0);
    EXPECT_DOUBLE_EQ (parameters.minorRadius, 2.0);
    EXPECT_NEAR (parameters.rotation, 0.0, 1e-12);

    ASSERT_TRUE (ed::SetEllipseCenter (doc, ellipse, { -4, 7, 2 }, error)) << error;
    ASSERT_TRUE (ed::SetEllipseRadii (doc, ellipse, 6.0, 3.0, error)) << error;
    ASSERT_TRUE (ed::SetEllipseRotation (doc, ellipse, 0.37, error)) << error;
    ASSERT_TRUE (ed::GetEllipseParameters (*ed::Find (doc, ellipse), parameters, error)) << error;
    EXPECT_EQ (parameters.center, (ed::Vec3 { -4, 7, 2 }));
    EXPECT_DOUBLE_EQ (parameters.majorRadius, 6.0);
    EXPECT_DOUBLE_EQ (parameters.minorRadius, 3.0);
    EXPECT_NEAR (parameters.rotation, 0.37, 1e-12);

    auto circleInput = ellipseInput;
    circleInput.uniform = true;
    const ed::Id circle = ed::AddPrimitive (doc, circleInput, "circle", error);
    ASSERT_NE (circle, ed::kNoId) << error;
    const auto circleBefore = *ed::Find (doc, circle);
    EXPECT_FALSE (ed::SetEllipseRadii (doc, circle, 5.0, 4.0, error));
    EXPECT_EQ (*ed::Find (doc, circle), circleBefore);
    EXPECT_FALSE (ed::SetEllipseMode (doc, ellipse, ed::EllipseMode::Circle, error));
    ASSERT_TRUE (ed::SetEllipseRadii (doc, ellipse, 4.0, 4.0, error)) << error;
    ASSERT_TRUE (ed::SetEllipseMode (doc, ellipse, ed::EllipseMode::Circle, error)) << error;
    EXPECT_EQ (ed::Find (doc, ellipse)->ellipse->mode, ed::EllipseMode::Circle);
    ASSERT_TRUE (ed::SetEllipseMode (doc, ellipse, ed::EllipseMode::Ellipse, error)) << error;
    EXPECT_EQ (ed::Find (doc, ellipse)->ellipse->mode, ed::EllipseMode::Ellipse);

    ed::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (ed::Document& d, std::string& why) {
        return ed::SetEllipseRadii (d, ellipse, 8.0, 4.0, why);
    })) << session.Message ();
    ASSERT_TRUE (ed::GetEllipseParameters (*ed::Find (session.Doc (), ellipse), parameters, error));
    EXPECT_DOUBLE_EQ (parameters.majorRadius, 8.0);
    ASSERT_TRUE (session.Undo ());
    ASSERT_TRUE (ed::GetEllipseParameters (*ed::Find (session.Doc (), ellipse), parameters, error));
    EXPECT_DOUBLE_EQ (parameters.majorRadius, 4.0);
    ASSERT_TRUE (session.Redo ());
    ASSERT_TRUE (ed::GetEllipseParameters (*ed::Find (session.Doc (), ellipse), parameters, error));
    EXPECT_DOUBLE_EQ (parameters.majorRadius, 8.0);
}

TEST (EditEllipse, EccentricAxesRetainTheMinorRadius)
{
    ed::Document doc;
    std::string error;
    const auto id =
        ed::AddPrimitive (doc, Input (ed::Primitive::Ellipse, {}, { 1000, 0.00001, 0 }), "eccentric", error);
    ASSERT_NE (id, ed::kNoId) << error;
    ed::EllipseParameters parameters;
    ASSERT_TRUE (ed::GetEllipseParameters (*ed::Find (doc, id), parameters, error)) << error;
    EXPECT_DOUBLE_EQ (parameters.majorRadius, 1000.0);
    EXPECT_NEAR (parameters.minorRadius, 0.00001, 1.0e-18);
    ASSERT_TRUE (ed::SetEllipseRadii (doc, id, 1000, ed::kMinSegment, error)) << error;
    ASSERT_TRUE (ed::GetEllipseParameters (*ed::Find (doc, id), parameters, error)) << error;
    EXPECT_NEAR (parameters.minorRadius, ed::kMinSegment, 1.0e-18);
}

TEST (EditEllipse, InvalidAndLockedParameterEditsDoNotMutateTheEntity)
{
    ed::Document doc;
    std::string error;
    const auto id = ed::AddPrimitive (doc, Input (ed::Primitive::Ellipse, {}, { 4, 2, 0 }), "locked", error);
    ASSERT_NE (id, ed::kNoId) << error;
    auto* entity = ed::Find (doc, id);
    entity->vertices.front ().lockXY = true;
    const auto original = *entity;
    EXPECT_FALSE (ed::SetEllipseCenter (doc, id, { 1, 0, 0 }, error));
    EXPECT_EQ (*entity, original);
    EXPECT_FALSE (ed::SetEllipseRadii (doc, id, 5, 2, error));
    EXPECT_EQ (*entity, original);
    EXPECT_FALSE (ed::SetEllipseRadii (doc, id, 4, 0, error));
    EXPECT_EQ (*entity, original);
    EXPECT_FALSE (ed::SetEllipseRotation (doc, id, 0.5, error));
    EXPECT_EQ (*entity, original);
}

TEST (EditEllipse, UnsupportedNodeHandleAndTopologyEditsRefuseUntilExplicitConversion)
{
    ed::Document doc;
    auto in = Input (ed::Primitive::Ellipse, {}, { 4, 2, 0 });
    std::string error;
    const ed::Id id = ed::AddPrimitive (doc, in, "test", error);
    ASSERT_NE (id, ed::kNoId);
    const ed::Entity original = *ed::Find (doc, id);
    const ed::Id vertex = original.vertices.front ().id, segment = original.segments.front ().id;
    EXPECT_FALSE (ed::MoveVertex (doc, id, vertex, { 5, 0, 0 }, error));
    EXPECT_EQ (*ed::Find (doc, id), original);
    EXPECT_FALSE (ed::SetHandle (doc, id, vertex, true, { 1, 0, 0 }, false, error));
    EXPECT_EQ (*ed::Find (doc, id), original);
    EXPECT_FALSE (ed::InsertVertex (doc, id, segment, { 3, 0, 0 }, error));
    EXPECT_EQ (*ed::Find (doc, id), original);
    EXPECT_FALSE (ed::SetSegmentCurved (doc, id, segment, false, error));
    EXPECT_EQ (*ed::Find (doc, id), original);
    EXPECT_FALSE (ed::SetCoordinate (doc, { { id, ed::Part::Vertex, vertex } }, 0, 5.0, error));
    EXPECT_EQ (*ed::Find (doc, id), original);
    const ed::Operation partial = ed::TransformOperation ({ { id, ed::Part::Vertex, vertex } }, {});
    EXPECT_FALSE (partial (doc, error));
    EXPECT_EQ (*ed::Find (doc, id), original);

    ed::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (ed::Document& d, std::string& why) {
        return ed::ConvertEllipseToFreeform (d, id, why);
    })) << session.Message ();
    EXPECT_FALSE (ed::Find (session.Doc (), id)->ellipse.has_value ());
    ASSERT_TRUE (session.Undo ());
    EXPECT_TRUE (ed::Find (session.Doc (), id)->ellipse.has_value ());
    ASSERT_TRUE (session.Redo ());
    EXPECT_FALSE (ed::Find (session.Doc (), id)->ellipse.has_value ());
    ASSERT_TRUE (session.Apply ([=] (ed::Document& d, std::string& why) {
        return ed::MoveVertex (d, id, vertex, { 5, 0, 0 }, why);
    })) << session.Message ();
}
