#include "Annotation/DrawList.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

using geomsrv::annotation::ArchitecturalAngleGlyph;
using geomsrv::annotation::BuildArchitecturalAngleGlyph;
using geomsrv::annotation::BuildArrowheadLegs;
using geomsrv::annotation::DrawList;
using geomsrv::annotation::Frame;
using geomsrv::annotation::IsDrawable;
using geomsrv::annotation::Node;
using geomsrv::annotation::Point3;
using geomsrv::annotation::Primitive;
using geomsrv::annotation::PrimitiveKind;
using geomsrv::annotation::RoleColour;
using geomsrv::annotation::SampleAngleArc;
using geomsrv::annotation::ScreenPoint;
using geomsrv::annotation::SemanticRole;

namespace {

constexpr double kPi = 3.14159265358979323846;

Primitive MakePrimitive (PrimitiveKind kind, std::vector<Point3> points)
{
    Primitive primitive;
    primitive.kind = kind;
    primitive.points = std::move (points);
    return primitive;
}

} // namespace

TEST (DrawList, SelectsOneNodeAndFrameWithoutChangingSelectionOnFailure)
{
    DrawList drawList;
    drawList.nodes = { Node { "first", { Frame {}, Frame {} } }, Node { "second", { Frame {} } } };

    ASSERT_TRUE (drawList.SelectFrame (0, 1));
    EXPECT_EQ (drawList.SelectedNode (), &drawList.nodes[0]);
    EXPECT_EQ (drawList.SelectedFrame (), &drawList.nodes[0].frames[1]);

    EXPECT_FALSE (drawList.SelectFrame (1, 2));
    EXPECT_EQ (drawList.SelectedNode (), &drawList.nodes[0]);
    EXPECT_EQ (drawList.SelectedFrame (), &drawList.nodes[0].frames[1]);

    ASSERT_TRUE (drawList.SelectNode (1));
    EXPECT_EQ (drawList.SelectedNode (), &drawList.nodes[1]);
    EXPECT_EQ (drawList.SelectedFrame (), nullptr);
}

TEST (DrawList, ClearSelectionAndMutationCannotExposeStalePointers)
{
    DrawList drawList;
    drawList.nodes = { Node { "node", { Frame {} } } };
    ASSERT_TRUE (drawList.SelectFrame (0, 0));

    drawList.nodes.clear ();
    EXPECT_EQ (drawList.SelectedNode (), nullptr);
    EXPECT_EQ (drawList.SelectedFrame (), nullptr);

    drawList.ClearSelection ();
    EXPECT_EQ (drawList.SelectedNode (), nullptr);
}

TEST (DrawList, BoundsContainEveryDrawablePointAcrossNodesAndFrames)
{
    DrawList drawList;
    Frame first;
    first.primitives.push_back (MakePrimitive (PrimitiveKind::Polyline, { { -2.0, 3.0, 1.0 }, { 4.0, -1.0, 5.0 } }));
    Frame second;
    second.primitives.push_back (MakePrimitive (PrimitiveKind::Point, { { 1.0, 8.0, -3.0 } }));
    drawList.nodes = { Node { "first", { first } }, Node { "second", { second } } };

    Point3 minimum;
    Point3 maximum;
    ASSERT_TRUE (drawList.GetBounds (minimum, maximum));
    EXPECT_DOUBLE_EQ (minimum.x, -2.0);
    EXPECT_DOUBLE_EQ (minimum.y, -1.0);
    EXPECT_DOUBLE_EQ (minimum.z, -3.0);
    EXPECT_DOUBLE_EQ (maximum.x, 4.0);
    EXPECT_DOUBLE_EQ (maximum.y, 8.0);
    EXPECT_DOUBLE_EQ (maximum.z, 5.0);
}

TEST (DrawList, BoundsSkipWholeInvalidPrimitivesAndPreserveOutputsWhenEmpty)
{
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    DrawList drawList;
    Frame frame;
    frame.primitives.push_back (MakePrimitive (PrimitiveKind::Polyline, { { -100.0, 0.0, 0.0 } }));
    frame.primitives.push_back (MakePrimitive (PrimitiveKind::Arrow, { { -50.0, 0.0, 0.0 }, { nan, 0.0, 0.0 } }));
    drawList.nodes = { Node { "node", { frame } } };

    Point3 minimum { 7.0, 8.0, 9.0 };
    Point3 maximum { 10.0, 11.0, 12.0 };
    EXPECT_FALSE (drawList.GetBounds (minimum, maximum));
    EXPECT_DOUBLE_EQ (minimum.x, 7.0);
    EXPECT_DOUBLE_EQ (maximum.z, 12.0);
    EXPECT_FALSE (IsDrawable (frame.primitives[0]));
    EXPECT_FALSE (IsDrawable (frame.primitives[1]));
}

TEST (DrawList, EveryGeometricPrimitiveKindHasAnExplicitMinimumGeometry)
{
    const Point3 points[] = { { 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 } };
    EXPECT_TRUE (IsDrawable (MakePrimitive (PrimitiveKind::Point, { points[0] })));
    EXPECT_TRUE (IsDrawable (MakePrimitive (PrimitiveKind::Label, { points[0] })));
    EXPECT_TRUE (IsDrawable (MakePrimitive (PrimitiveKind::Polyline, { points[0], points[1] })));
    EXPECT_TRUE (IsDrawable (MakePrimitive (PrimitiveKind::Arrow, { points[0], points[1] })));
    EXPECT_TRUE (IsDrawable (MakePrimitive (PrimitiveKind::Dimension, { points[0], points[1] })));
    EXPECT_TRUE (IsDrawable (MakePrimitive (PrimitiveKind::Angle, { points[0], points[1], points[2] })));
}

TEST (DrawList, ModelCarriesRendererPayloadAndElementUsesGuidInsteadOfGeometry)
{
    Node node;
    node.name = "wall clearance";

    Primitive primitive;
    primitive.kind = PrimitiveKind::Element;
    primitive.role = SemanticRole::Modify;
    primitive.text = "300 mm";
    primitive.guid = "d2f6d950-31c8-4d23-a095-a24ec27648ea";
    primitive.closed = true;
    primitive.direction = true;
    primitive.offset = 0.3;

    EXPECT_EQ (node.name, "wall clearance");
    EXPECT_TRUE (primitive.points.empty ());
    EXPECT_TRUE (IsDrawable (primitive));
    EXPECT_EQ (primitive.role, SemanticRole::Modify);
    EXPECT_EQ (primitive.text, "300 mm");
    EXPECT_TRUE (primitive.closed);
    EXPECT_TRUE (primitive.direction);
    EXPECT_DOUBLE_EQ (primitive.offset, 0.3);

    primitive.guid.clear ();
    EXPECT_FALSE (IsDrawable (primitive));
}

TEST (DrawList, NonfinitePrimitiveOffsetIsNotDrawable)
{
    Primitive primitive = MakePrimitive (PrimitiveKind::Point, { { 0.0, 0.0, 0.0 } });
    primitive.offset = std::numeric_limits<double>::infinity ();
    EXPECT_FALSE (IsDrawable (primitive));
}

TEST (DrawList, ArrowheadLegsAreSymmetricInTheRequestedPlane)
{
    std::vector<Point3> legs;
    ASSERT_TRUE (BuildArrowheadLegs ({ 0.0, 0.0, 0.0 }, { 2.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, kPi / 6.0, legs));

    ASSERT_EQ (legs.size (), 4u);
    EXPECT_DOUBLE_EQ (legs[0].x, 2.0);
    EXPECT_DOUBLE_EQ (legs[2].x, 2.0);
    EXPECT_NEAR (legs[1].x, 2.0 - std::cos (kPi / 6.0), 1.0e-12);
    EXPECT_NEAR (legs[3].x, legs[1].x, 1.0e-12);
    EXPECT_NEAR (legs[1].y, -legs[3].y, 1.0e-12);
    EXPECT_NEAR (std::hypot (legs[1].x - 2.0, legs[1].y), 1.0, 1.0e-12);
}

TEST (DrawList, ArrowheadRefusesDegenerateAndNonPlanarGeometryTransactionally)
{
    std::vector<Point3> legs = { { 9.0, 9.0, 9.0 } };
    EXPECT_FALSE (BuildArrowheadLegs ({ 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, 0.5, legs));
    EXPECT_FALSE (BuildArrowheadLegs ({ 0.0, 0.0, 1.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, 0.5, legs));
    EXPECT_FALSE (BuildArrowheadLegs ({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 },
                                      std::numeric_limits<double>::infinity (), 0.5, legs));
    ASSERT_EQ (legs.size (), 1u);
    EXPECT_DOUBLE_EQ (legs[0].x, 9.0);
}

TEST (DrawList, AngleArcSamplesExactQuarterCircleIncludingEndpoints)
{
    std::vector<Point3> arc;
    ASSERT_TRUE (
        SampleAngleArc ({ 1.0, 2.0, 3.0 }, { 3.0, 2.0, 3.0 }, { 1.0, 5.0, 3.0 }, { 0.0, 0.0, 1.0 }, 2.0, 4, arc));

    ASSERT_EQ (arc.size (), 5u);
    EXPECT_NEAR (arc.front ().x, 3.0, 1.0e-12);
    EXPECT_NEAR (arc.front ().y, 2.0, 1.0e-12);
    EXPECT_NEAR (arc.back ().x, 1.0, 1.0e-12);
    EXPECT_NEAR (arc.back ().y, 4.0, 1.0e-12);
    for (const Point3& point : arc) {
        EXPECT_NEAR (std::hypot (point.x - 1.0, point.y - 2.0), 2.0, 1.0e-12);
        EXPECT_NEAR (point.z, 3.0, 1.0e-12);
    }
}

TEST (DrawList, MinorAngleArcIsStableWhenThePlaneNormalIsReversed)
{
    std::vector<Point3> positive;
    std::vector<Point3> negative;
    ASSERT_TRUE (
        SampleAngleArc ({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, 2, positive));
    ASSERT_TRUE (
        SampleAngleArc ({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, -1.0 }, 1.0, 2, negative));

    ASSERT_EQ (positive.size (), negative.size ());
    for (size_t index = 0; index < positive.size (); ++index) {
        EXPECT_NEAR (positive[index].x, negative[index].x, 1.0e-12);
        EXPECT_NEAR (positive[index].y, negative[index].y, 1.0e-12);
        EXPECT_NEAR (positive[index].z, negative[index].z, 1.0e-12);
    }
}

TEST (DrawList, AngleArcRefusesInvalidGeometryTransactionally)
{
    std::vector<Point3> arc = { { 9.0, 9.0, 9.0 } };
    EXPECT_FALSE (
        SampleAngleArc ({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 2.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, 4, arc));
    EXPECT_FALSE (
        SampleAngleArc ({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 1.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, 4, arc));
    EXPECT_FALSE (
        SampleAngleArc ({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, 0, arc));
    EXPECT_FALSE (
        SampleAngleArc ({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 }, 1.0, 4097, arc));
    ASSERT_EQ (arc.size (), 1u);
    EXPECT_DOUBLE_EQ (arc[0].x, 9.0);
}

TEST (DrawList, CanonicalRolePaletteIncludesDarkRedDefault)
{
    const auto none = RoleColour (SemanticRole::None);
    EXPECT_EQ (none.red, 0x8B);
    EXPECT_EQ (none.green, 0x1E);
    EXPECT_EQ (none.blue, 0x1E);
    EXPECT_EQ (none.alpha, 0xFF);

    const uint32_t expected[] = { 0x8B1E1EFFu, 0x189C5CFFu, 0xDA4444FFu, 0xE8911CFFu, 0x5C6C7EFFu, 0x307ED6FFu };
    for (size_t index = 0; index < std::size (expected); ++index)
        EXPECT_EQ (geomsrv::annotation::PackRgba (RoleColour (static_cast<SemanticRole> (index))), expected[index]);
}

TEST (DrawList, ArchitecturalAngleGlyphHasExactEndpointsTangentHeadsAndOutsideLabel)
{
    ArchitecturalAngleGlyph glyph;
    ASSERT_TRUE (BuildArchitecturalAngleGlyph ({ 100.0, 100.0 }, { 200.0, 100.0 }, { 100.0, 0.0 }, 1.0, glyph));

    ASSERT_EQ (glyph.arc.size (), 33u);
    EXPECT_NEAR (glyph.arc.front ().x, 130.0, 1.0e-12);
    EXPECT_NEAR (glyph.arc.front ().y, 100.0, 1.0e-12);
    EXPECT_NEAR (glyph.arc.back ().x, 100.0, 1.0e-12);
    EXPECT_NEAR (glyph.arc.back ().y, 70.0, 1.0e-12);
    EXPECT_DOUBLE_EQ (glyph.degrees, 90.0);
    EXPECT_NEAR (glyph.labelAnchor.x, 100.0 + 30.0 / std::sqrt (2.0), 1.0e-12);
    EXPECT_DOUBLE_EQ (glyph.labelAnchor.y, 112.0);

    for (size_t index = 0; index < 2; ++index) {
        const auto& arrow = glyph.arrowheads[index];
        const ScreenPoint base { (arrow.second.x + arrow.third.x) * 0.5, (arrow.second.y + arrow.third.y) * 0.5 };
        const ScreenPoint radial { arrow.first.x - 100.0, arrow.first.y - 100.0 };
        const ScreenPoint axis { arrow.first.x - base.x, arrow.first.y - base.y };
        EXPECT_NEAR (radial.x * axis.x + radial.y * axis.y, 0.0, 1.0e-10);
        EXPECT_NEAR (std::hypot (axis.x, axis.y), 8.0, 1.0e-12);
        EXPECT_NEAR (std::hypot (arrow.second.x - arrow.third.x, arrow.second.y - arrow.third.y), 7.0, 1.0e-12);
    }
}

TEST (DrawList, ArchitecturalAngleGlyphMetricsScaleWithDpiButAngleDoesNot)
{
    ArchitecturalAngleGlyph one;
    ArchitecturalAngleGlyph two;
    ASSERT_TRUE (BuildArchitecturalAngleGlyph ({ 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 }, 1.0, one));
    ASSERT_TRUE (BuildArchitecturalAngleGlyph ({ 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 }, 2.0, two));

    EXPECT_DOUBLE_EQ (two.arc.front ().x, one.arc.front ().x * 2.0);
    EXPECT_DOUBLE_EQ (two.labelAnchor.y, one.labelAnchor.y * 2.0);
    EXPECT_DOUBLE_EQ (two.arcWidthPixels, one.arcWidthPixels * 2.0);
    EXPECT_DOUBLE_EQ (two.fontSizePixels, one.fontSizePixels * 2.0);
    EXPECT_DOUBLE_EQ (two.degrees, one.degrees);
}
