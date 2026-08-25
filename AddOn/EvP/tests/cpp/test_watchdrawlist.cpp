#include "Preview/RetainedTraceSelection.hpp"

#include <gtest/gtest.h>

using geomsrv::annotation::FitFrame;
using geomsrv::annotation::PrimitiveKind;
using geomsrv::annotation::SemanticRole;
using geomsrv::annotation::Transform2D;

TEST (WatchDrawList, ConvertsFrameIdentityGeometryAndSemantics)
{
    evp::preview::WatchPrimitive source;
    source.kind = evp::preview::WatchPrimitiveKind::Polyline;
    source.points = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 };
    source.text = "route";
    source.role = "guide";
    source.closed = true;
    source.direction = true;
    source.offset = 0.25;

    evp::preview::WatchFrame sourceFrame;
    sourceFrame.index = 42;
    sourceFrame.primitives.push_back (source);
    evp::preview::WatchNode sourceNode;
    sourceNode.name = "clearance";
    sourceNode.frames.push_back (sourceFrame);
    evp::preview::WatchTrace trace;
    trace.nodes.push_back (sourceNode);

    auto drawList = evp::preview::ToDrawList (trace);
    ASSERT_EQ (drawList.nodes.size (), 1u);
    ASSERT_EQ (drawList.nodes[0].frames.size (), 1u);
    const auto& frame = drawList.nodes[0].frames[0];
    ASSERT_EQ (frame.primitives.size (), 1u);
    const auto& primitive = frame.primitives[0];
    EXPECT_EQ (frame.index, 42u);
    EXPECT_EQ (primitive.kind, PrimitiveKind::Polyline);
    EXPECT_EQ (primitive.role, SemanticRole::Guide);
    EXPECT_EQ (primitive.points.size (), 2u);
    EXPECT_EQ (primitive.text, "route");
    EXPECT_TRUE (primitive.closed);
    EXPECT_TRUE (primitive.direction);
    EXPECT_DOUBLE_EQ (primitive.offset, 0.25);
}

TEST (WatchDrawList, UnknownRoleFallsBackWithoutDroppingPrimitive)
{
    evp::preview::WatchPrimitive source;
    source.kind = evp::preview::WatchPrimitiveKind::Point;
    source.points = { 0.0, 0.0, 0.0 };
    source.role = "future-role";
    evp::preview::WatchFrame frame;
    frame.primitives.push_back (source);
    evp::preview::WatchNode node;
    node.name = "node";
    node.frames.push_back (frame);
    evp::preview::WatchTrace trace;
    trace.nodes.push_back (node);

    const auto drawList = evp::preview::ToDrawList (trace);
    EXPECT_EQ (drawList.nodes[0].frames[0].primitives[0].role, SemanticRole::None);
}

TEST (WatchDrawList, FitsSelectedFrameWithUniformScaleAndYUpProjection)
{
    geomsrv::annotation::Frame frame;
    geomsrv::annotation::Primitive line;
    line.kind = PrimitiveKind::Polyline;
    line.points = { { 10.0, 20.0, 0.0 }, { 30.0, 30.0, 0.0 } };
    frame.primitives.push_back (line);

    Transform2D transform;
    ASSERT_TRUE (FitFrame (frame, 240.0, 140.0, 20.0, transform));
    EXPECT_DOUBLE_EQ (transform.scaleX, 10.0);
    EXPECT_DOUBLE_EQ (transform.scaleY, -10.0);
    EXPECT_DOUBLE_EQ (transform.offX, -80.0);
    EXPECT_DOUBLE_EQ (transform.offY, 320.0);
}

TEST (WatchDrawList, FitRefusalLeavesTransformUnchanged)
{
    geomsrv::annotation::Frame frame;
    Transform2D transform { 2.0, 3.0, 4.0, 5.0 };
    EXPECT_FALSE (FitFrame (frame, 100.0, 100.0, 10.0, transform));
    EXPECT_DOUBLE_EQ (transform.scaleX, 2.0);
    EXPECT_DOUBLE_EQ (transform.offY, 5.0);
}
