#include "Edit/EditPrimitives.hpp"
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
