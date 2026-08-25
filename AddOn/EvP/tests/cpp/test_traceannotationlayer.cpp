#include "ArchViz/TraceAnnotationLayer.hpp"

#include <gtest/gtest.h>

namespace {

namespace annotation = geomsrv::annotation;
namespace archviz = geomsrv::archviz;

constexpr float kIdentity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};

annotation::Primitive Primitive (annotation::PrimitiveKind kind, std::vector<annotation::Point3> points)
{
    annotation::Primitive primitive;
    primitive.kind = kind;
    primitive.points = std::move (points);
    return primitive;
}

TEST (TraceAnnotationLayer, ProjectsWorldLinesWithRendererCoordinateConventions)
{
    annotation::Frame frame;
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Polyline, { { -0.5, 0.5, 0.5 }, { 0.5, -0.5, 0.5 } }));

    const archviz::ProjectedDrawList draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    ASSERT_EQ (draw.lines.size (), 1u);
    EXPECT_FLOAT_EQ (draw.lines[0].from.x, 50.0f);
    EXPECT_FLOAT_EQ (draw.lines[0].from.y, 25.0f);
    EXPECT_FLOAT_EQ (draw.lines[0].to.x, 150.0f);
    EXPECT_FLOAT_EQ (draw.lines[0].to.y, 75.0f);
}

TEST (TraceAnnotationLayer, BuildsArrowDimensionAngleAndProjectedLabels)
{
    annotation::Frame frame;
    auto arrow = Primitive (annotation::PrimitiveKind::Arrow, { { -0.8, 0.0, 0.5 }, { -0.2, 0.0, 0.5 } });
    arrow.text = "flow";
    frame.primitives.push_back (std::move (arrow));
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Dimension, { { -0.5, -0.5, 0.5 }, { 0.5, -0.5, 0.5 } }));
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Angle, { { 0.0, 0.0, 0.5 }, { 0.5, 0.0, 0.5 }, { 0.0, 0.5, 0.5 } }));
    auto label = Primitive (annotation::PrimitiveKind::Label, { { 0.25, 0.25, 0.5 } });
    label.text = "anchor";
    frame.primitives.push_back (std::move (label));

    const archviz::ProjectedDrawList draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    EXPECT_GE (draw.lines.size (), 36u);
    EXPECT_EQ (draw.triangles.size (), 5u);
    ASSERT_EQ (draw.labels.size (), 4u);
    EXPECT_EQ (draw.labels[0].text, "flow");
    EXPECT_EQ (draw.labels[1].text, "1.000 m");
    EXPECT_EQ (draw.labels[2].text, "90.0\xC2\xB0");
    EXPECT_TRUE (draw.labels[2].centered);
    EXPECT_FLOAT_EQ (draw.labels[2].fontSize, 18.0f);
    EXPECT_EQ (draw.labels[3].text, "anchor");
}

TEST (TraceAnnotationLayer, AngleUsesSharedMinorArcRadialLabelFilledHeadsAndDarkRedStyle)
{
    annotation::Frame frame;
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Angle, { { 0.0, 0.0, 0.5 }, { 0.5, 0.0, 0.5 }, { 0.0, 0.5, 0.5 } }));

    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    ASSERT_EQ (draw.lines.size (), 32u);
    ASSERT_EQ (draw.triangles.size (), 2u);
    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_FLOAT_EQ (draw.lines.front ().from.x, 130.0f);
    EXPECT_FLOAT_EQ (draw.lines.front ().from.y, 50.0f);
    EXPECT_FLOAT_EQ (draw.lines.back ().to.x, 100.0f);
    EXPECT_FLOAT_EQ (draw.lines.back ().to.y, 20.0f);
    EXPECT_NEAR (draw.labels[0].anchor.x, 100.0f + 42.0f / std::sqrt (2.0f), 1.0e-5f);
    EXPECT_NEAR (draw.labels[0].anchor.y, 50.0f - 42.0f / std::sqrt (2.0f), 1.0e-5f);
    EXPECT_EQ (draw.lines[0].rgba, 0x8B1E1EFFu);
    EXPECT_EQ (draw.triangles[0].rgba, 0x8B1E1EFFu);
    EXPECT_EQ (draw.labels[0].rgba, 0x8B1E1EFFu);
}

TEST (TraceAnnotationLayer, SwappedAngleOrderReversesArcAndKeepsRadialLabel)
{
    annotation::Frame forwardFrame;
    auto angle =
        Primitive (annotation::PrimitiveKind::Angle, { { 0.0, 0.0, 0.5 }, { 0.5, 0.0, 0.5 }, { 0.0, 0.5, 0.5 } });
    forwardFrame.primitives.push_back (angle);
    annotation::Frame reverseFrame;
    angle.direction = true;
    reverseFrame.primitives.push_back (angle);

    const auto forward = archviz::BuildTraceAnnotations (forwardFrame, kIdentity, 200, 100);
    const auto reverse = archviz::BuildTraceAnnotations (reverseFrame, kIdentity, 200, 100);

    ASSERT_EQ (forward.lines.size (), reverse.lines.size ());
    for (size_t index = 0; index < forward.lines.size (); ++index) {
        const auto& left = forward.lines[index];
        const auto& right = reverse.lines[reverse.lines.size () - 1 - index];
        EXPECT_NEAR (left.from.x, right.to.x, 1.0e-5f);
        EXPECT_NEAR (left.from.y, right.to.y, 1.0e-5f);
        EXPECT_NEAR (left.to.x, right.from.x, 1.0e-5f);
        EXPECT_NEAR (left.to.y, right.from.y, 1.0e-5f);
    }
    ASSERT_EQ (forward.labels.size (), 1u);
    ASSERT_EQ (reverse.labels.size (), 1u);
    EXPECT_NEAR (forward.labels[0].anchor.x, reverse.labels[0].anchor.x, 1.0e-5f);
    EXPECT_NEAR (forward.labels[0].anchor.y, reverse.labels[0].anchor.y, 1.0e-5f);
}

TEST (TraceAnnotationLayer, UsesRoleColourForLinesTrianglesAndText)
{
    annotation::Frame frame;
    auto arrow = Primitive (annotation::PrimitiveKind::Arrow, { { -0.5, 0.0, 0.5 }, { 0.5, 0.0, 0.5 } });
    arrow.role = annotation::SemanticRole::None;
    arrow.text = "direction";
    frame.primitives.push_back (arrow);

    const archviz::ProjectedDrawList draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    ASSERT_EQ (draw.lines.size (), 1u);
    ASSERT_EQ (draw.triangles.size (), 1u);
    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_EQ (draw.lines[0].rgba, 0x8B1E1EFFu);
    EXPECT_EQ (draw.triangles[0].rgba, 0x8B1E1EFFu);
    EXPECT_EQ (draw.labels[0].rgba, 0x8B1E1EFFu);
}

TEST (TraceAnnotationLayer, DpiScalesFurnitureWithoutMovingProjectedGeometry)
{
    annotation::Frame frame;
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Arrow, { { -0.5, 0.0, 0.5 }, { 0.5, 0.0, 0.5 } }));

    const auto one = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100, 1.0f);
    const auto two = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100, 2.0f);

    ASSERT_EQ (one.lines.size (), 1u);
    ASSERT_EQ (two.lines.size (), 1u);
    ASSERT_EQ (one.triangles.size (), 1u);
    ASSERT_EQ (two.triangles.size (), 1u);
    EXPECT_FLOAT_EQ (one.lines[0].from.x, two.lines[0].from.x);
    EXPECT_FLOAT_EQ (one.lines[0].to.x, two.lines[0].to.x);
    const auto baseDistance = [] (const archviz::ScreenTriangle& triangle) {
        const float baseX = (triangle.points[1].x + triangle.points[2].x) * 0.5f;
        const float baseY = (triangle.points[1].y + triangle.points[2].y) * 0.5f;
        return std::hypot (triangle.points[0].x - baseX, triangle.points[0].y - baseY);
    };
    EXPECT_NEAR (baseDistance (two.triangles[0]), baseDistance (one.triangles[0]) * 2.0f, 1.0e-5f);
}

TEST (TraceAnnotationLayer, DpiScalesAngleRadiusGapWidthFontAndTangentHeads)
{
    annotation::Frame frame;
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Angle, { { 0.0, 0.0, 0.5 }, { 0.5, 0.0, 0.5 }, { 0.0, 0.5, 0.5 } }));

    const auto one = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100, 1.0f);
    const auto two = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100, 2.0f);

    ASSERT_EQ (one.lines.size (), 32u);
    ASSERT_EQ (two.lines.size (), 32u);
    ASSERT_EQ (one.triangles.size (), 2u);
    ASSERT_EQ (two.triangles.size (), 2u);
    ASSERT_EQ (one.labels.size (), 1u);
    ASSERT_EQ (two.labels.size (), 1u);
    EXPECT_FLOAT_EQ (one.lines[0].width * 2.0f, two.lines[0].width);
    EXPECT_FLOAT_EQ (one.labels[0].fontSize * 2.0f, two.labels[0].fontSize);
    EXPECT_NEAR (two.lines.front ().from.x - 100.0f, (one.lines.front ().from.x - 100.0f) * 2.0f, 1.0e-5f);
    EXPECT_NEAR (two.labels[0].anchor.x - 100.0f, (one.labels[0].anchor.x - 100.0f) * 2.0f, 1.0e-5f);
    EXPECT_NEAR (two.labels[0].anchor.y - 50.0f, (one.labels[0].anchor.y - 50.0f) * 2.0f, 1.0e-5f);
    for (size_t index = 0; index < 2; ++index) {
        const auto axisLength = [] (const archviz::ScreenTriangle& triangle) {
            const float baseX = (triangle.points[1].x + triangle.points[2].x) * 0.5f;
            const float baseY = (triangle.points[1].y + triangle.points[2].y) * 0.5f;
            return std::hypot (triangle.points[0].x - baseX, triangle.points[0].y - baseY);
        };
        EXPECT_NEAR (axisLength (two.triangles[index]), axisLength (one.triangles[index]) * 2.0f, 1.0e-5f);
    }
}

TEST (TraceAnnotationLayer, FitsOnlyTheFramePassedToDiligentProjection)
{
    annotation::Frame selected;
    selected.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Polyline, { { -0.1, 0.0, 0.5 }, { 0.1, 0.0, 0.5 } }));

    float fitted[16];
    ASSERT_TRUE (archviz::FitFrameProjection (selected, kIdentity, 200, 100, 20.0f, fitted));
    const auto draw = archviz::BuildTraceAnnotations (selected, kIdentity, 200, 100, 1.0f, true);

    ASSERT_EQ (draw.lines.size (), 1u);
    EXPECT_NEAR (draw.lines[0].from.x, 24.0f, 1.0e-4f);
    EXPECT_NEAR (draw.lines[0].to.x, 176.0f, 1.0e-4f);
    EXPECT_NEAR (draw.lines[0].from.y, 50.0f, 1.0e-4f);
    EXPECT_NEAR (draw.lines[0].to.y, 50.0f, 1.0e-4f);
}

TEST (TraceAnnotationLayer, RejectsPointsBehindTheD3DClipVolume)
{
    annotation::Frame frame;
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Polyline, { { -0.5, 0.0, -0.1 }, { 0.5, 0.0, -0.1 } }));

    const archviz::ProjectedDrawList draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    EXPECT_TRUE (draw.lines.empty ());
    EXPECT_TRUE (draw.labels.empty ());
}

} // namespace
