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

    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 400, 300);

    ASSERT_EQ (draw.lines.size (), 32u);
    ASSERT_EQ (draw.triangles.size (), 2u);
    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_FLOAT_EQ (draw.lines.front ().from.x, 230.0f);
    EXPECT_FLOAT_EQ (draw.lines.front ().from.y, 150.0f);
    EXPECT_FLOAT_EQ (draw.lines.back ().to.x, 200.0f);
    EXPECT_FLOAT_EQ (draw.lines.back ().to.y, 120.0f);
    EXPECT_GT (std::hypot (draw.labels[0].anchor.x - 200.0f,
                           draw.labels[0].anchor.y + draw.labels[0].fontSize * 0.5f - 150.0f),
               55.0f);
    EXPECT_NEAR (draw.labels[0].rotationRadians, -3.14159265358979323846f / 4.0f, 1.0e-5f);
    EXPECT_FALSE (draw.labels[0].backgroundPanel);
    EXPECT_EQ (draw.lines[0].rgba, 0x8B1E1EFFu);
    EXPECT_EQ (draw.triangles[0].rgba, 0x8B1E1EFFu);
    EXPECT_EQ (draw.labels[0].rgba, 0x8B1E1EFFu);
}

TEST (TraceAnnotationLayer, AngleValueComesFromModelRaysInsteadOfProjectedRays)
{
    annotation::Frame frame;
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Angle, { { 0.0, 0.0, 0.5 }, { 0.5, 0.0, 0.5 }, { 0.5, 0.5, 0.5 } }));

    // The 2:1 surface aspect projects the second ray to about 26.6 degrees.
    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_EQ (draw.labels[0].text, "45.0\xC2\xB0");
}

TEST (TraceAnnotationLayer, AngleRadialLabelFlipsToRemainUpright)
{
    annotation::Frame frame;
    frame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Angle, { { 0.0, 0.0, 0.5 }, { -0.5, 0.0, 0.5 }, { 0.0, 0.5, 0.5 } }));

    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_NEAR (draw.labels[0].rotationRadians, 3.14159265358979323846f / 4.0f, 1.0e-5f);
}

TEST (TraceAnnotationLayer, FittingDimensionTextInterruptsLineAndUsesShapedExtent)
{
    annotation::Frame frame;
    auto dimension = Primitive (annotation::PrimitiveKind::Dimension, { { -0.5, 0.0, 0.5 }, { 0.5, 0.0, 0.5 } });
    dimension.text = "measured";
    frame.primitives.push_back (dimension);
    bool measured = false;
    const archviz::ScreenTextMeasure measure = [&measured] (std::string_view, float fontSize,
                                                            archviz::ScreenTextExtent& extent) {
        measured = true;
        extent = { 40.0f, fontSize };
        return true;
    };

    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100, 1.0f, false, measure);

    EXPECT_TRUE (measured);
    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_FLOAT_EQ (draw.labels[0].anchor.x, 100.0f);
    ASSERT_EQ (draw.lines.size (), 4u);
    const float dimensionY = draw.labels[0].anchor.y + 9.0f;
    std::vector<archviz::ScreenLine> dimensionLines;
    for (const auto& line : draw.lines) {
        if (std::fabs (line.from.y - dimensionY) < 1.0e-5f && std::fabs (line.to.y - dimensionY) < 1.0e-5f)
            dimensionLines.push_back (line);
    }
    ASSERT_EQ (dimensionLines.size (), 2u);
    EXPECT_LT (dimensionLines[0].to.x, draw.labels[0].anchor.x);
    EXPECT_GT (dimensionLines[1].from.x, draw.labels[0].anchor.x);
}

TEST (TraceAnnotationLayer, LinearDimensionTextFollowsProjectedLineAndRemainsUpright)
{
    annotation::Frame forwardFrame;
    forwardFrame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Dimension, { { -0.5, 0.5, 0.5 }, { 0.5, -0.5, 0.5 } }));
    annotation::Frame reverseFrame;
    reverseFrame.primitives.push_back (
        Primitive (annotation::PrimitiveKind::Dimension, { { 0.5, -0.5, 0.5 }, { -0.5, 0.5, 0.5 } }));

    const auto forward = archviz::BuildTraceAnnotations (forwardFrame, kIdentity, 400, 300);
    const auto reverse = archviz::BuildTraceAnnotations (reverseFrame, kIdentity, 400, 300);

    ASSERT_EQ (forward.labels.size (), 1u);
    ASSERT_EQ (reverse.labels.size (), 1u);
    EXPECT_NEAR (forward.labels[0].rotationRadians, std::atan2 (150.0f, 200.0f), 1.0e-5f);
    EXPECT_NEAR (reverse.labels[0].rotationRadians, forward.labels[0].rotationRadians, 1.0e-5f);
}

TEST (TraceAnnotationLayer, SampledDimensionDrawsOffsetArcWithTangentText)
{
    annotation::Frame frame;
    auto dimension = Primitive (annotation::PrimitiveKind::Dimension,
                                { { 0.5, 0.0, 0.5 }, { 0.353553390593, 0.353553390593, 0.5 }, { 0.0, 0.5, 0.5 } });
    dimension.offset = -0.1;
    dimension.text = "L 0.785 m";
    frame.primitives.push_back (std::move (dimension));

    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_NE (draw.labels[0].rotationRadians, 0.0f);
    ASSERT_GE (draw.lines.size (), 4u);
    EXPECT_FLOAT_EQ (draw.lines[0].from.x, 150.0f);
    EXPECT_GT (draw.lines[0].to.x, draw.lines[0].from.x);
    EXPECT_EQ (draw.triangles.size (), 2u);
}

TEST (TraceAnnotationLayer, ShortDimensionMovesTextOutsideAndFlipsArrows)
{
    annotation::Frame frame;
    auto dimension = Primitive (annotation::PrimitiveKind::Dimension, { { -0.02, 0.0, 0.5 }, { 0.02, 0.0, 0.5 } });
    dimension.text = "123.4 mm";
    frame.primitives.push_back (dimension);
    const archviz::ScreenTextMeasure measure = [] (std::string_view, float fontSize,
                                                   archviz::ScreenTextExtent& extent) {
        extent = { 72.0f, fontSize };
        return true;
    };

    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100, 1.0f, false, measure);

    ASSERT_EQ (draw.labels.size (), 1u);
    EXPECT_LT (draw.labels[0].anchor.x, 98.0f);
    ASSERT_EQ (draw.triangles.size (), 2u);
    const auto baseX = [] (const archviz::ScreenTriangle& triangle) {
        return (triangle.points[1].x + triangle.points[2].x) * 0.5f;
    };
    EXPECT_LT (draw.triangles[0].points[0].x, baseX (draw.triangles[0]));
    EXPECT_GT (draw.triangles[1].points[0].x, baseX (draw.triangles[1]));
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

    const auto forward = archviz::BuildTraceAnnotations (forwardFrame, kIdentity, 400, 300);
    const auto reverse = archviz::BuildTraceAnnotations (reverseFrame, kIdentity, 400, 300);

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

    const auto one = archviz::BuildTraceAnnotations (frame, kIdentity, 400, 300, 1.0f);
    const auto two = archviz::BuildTraceAnnotations (frame, kIdentity, 800, 600, 2.0f);

    ASSERT_EQ (one.lines.size (), 32u);
    ASSERT_EQ (two.lines.size (), 32u);
    ASSERT_EQ (one.triangles.size (), 2u);
    ASSERT_EQ (two.triangles.size (), 2u);
    ASSERT_EQ (one.labels.size (), 1u);
    ASSERT_EQ (two.labels.size (), 1u);
    EXPECT_FLOAT_EQ (one.lines[0].width * 2.0f, two.lines[0].width);
    EXPECT_FLOAT_EQ (one.labels[0].fontSize * 2.0f, two.labels[0].fontSize);
    EXPECT_NEAR (two.lines.front ().from.x - 400.0f, (one.lines.front ().from.x - 200.0f) * 2.0f, 1.0e-5f);
    EXPECT_NEAR (two.labels[0].anchor.x - 400.0f, (one.labels[0].anchor.x - 200.0f) * 2.0f, 1.0e-5f);
    EXPECT_NEAR (two.labels[0].anchor.y - 300.0f, (one.labels[0].anchor.y - 150.0f) * 2.0f, 1.0e-5f);
    for (size_t index = 0; index < 2; ++index) {
        const auto axisLength = [] (const archviz::ScreenTriangle& triangle) {
            const float baseX = (triangle.points[1].x + triangle.points[2].x) * 0.5f;
            const float baseY = (triangle.points[1].y + triangle.points[2].y) * 0.5f;
            return std::hypot (triangle.points[0].x - baseX, triangle.points[0].y - baseY);
        };
        EXPECT_NEAR (axisLength (two.triangles[index]), axisLength (one.triangles[index]) * 2.0f, 1.0e-5f);
    }
}

TEST (TraceAnnotationLayer, ZoomKeepsMeasurementFurnitureAtScreenSize)
{
    annotation::Frame frame;
    auto dimension = Primitive (annotation::PrimitiveKind::Dimension, { { -0.8, 0.0, 0.5 }, { 0.8, 0.0, 0.5 } });
    dimension.text = "1 m";
    frame.primitives.push_back (dimension);
    float zoomedOut[16];
    std::copy (kIdentity, kIdentity + 16, zoomedOut);
    zoomedOut[0] = zoomedOut[5] = 0.5f;

    const auto near = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);
    const auto far = archviz::BuildTraceAnnotations (frame, zoomedOut, 200, 100);

    ASSERT_GE (near.lines.size (), 2u);
    ASSERT_GE (far.lines.size (), 2u);
    ASSERT_EQ (near.labels.size (), 1u);
    ASSERT_EQ (far.labels.size (), 1u);
    ASSERT_EQ (near.triangles.size (), 2u);
    ASSERT_EQ (far.triangles.size (), 2u);
    const auto lineLength = [] (const archviz::ScreenLine& line) {
        return std::hypot (line.to.x - line.from.x, line.to.y - line.from.y);
    };
    const auto arrowLength = [] (const archviz::ScreenTriangle& triangle) {
        const float baseX = (triangle.points[1].x + triangle.points[2].x) * 0.5f;
        const float baseY = (triangle.points[1].y + triangle.points[2].y) * 0.5f;
        return std::hypot (triangle.points[0].x - baseX, triangle.points[0].y - baseY);
    };
    EXPECT_FLOAT_EQ (lineLength (near.lines[0]), lineLength (far.lines[0]));
    EXPECT_FLOAT_EQ (near.labels[0].fontSize, far.labels[0].fontSize);
    EXPECT_FLOAT_EQ (arrowLength (near.triangles[0]), arrowLength (far.triangles[0]));
    EXPECT_FALSE (near.labels[0].backgroundPanel);
    EXPECT_FALSE (far.labels[0].backgroundPanel);
}

TEST (TraceAnnotationLayer, OverlappingMeasurementLabelsUseBoundedCandidateAndLeader)
{
    annotation::Frame frame;
    const auto angle =
        Primitive (annotation::PrimitiveKind::Angle, { { 0.0, 0.0, 0.5 }, { 0.5, 0.0, 0.5 }, { 0.0, 0.5, 0.5 } });
    frame.primitives = { angle, angle };

    const auto draw = archviz::BuildTraceAnnotations (frame, kIdentity, 200, 100);

    ASSERT_EQ (draw.labels.size (), 2u);
    EXPECT_FALSE (draw.labels[0].backgroundPanel);
    EXPECT_FALSE (draw.labels[1].backgroundPanel);
    EXPECT_TRUE (draw.labels[0].anchor.x != draw.labels[1].anchor.x ||
                 draw.labels[0].anchor.y != draw.labels[1].anchor.y);
    EXPECT_GE (draw.lines.size (), 65u);
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
