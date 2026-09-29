// ArchViz/OverlayAnnotationContent: the Watch trace's frame as an overlay layer --
// each primitive kind as what the overlays draw of it, in its role's colour.

#include "ArchViz/OverlayAnnotationContent.hpp"

#include <gtest/gtest.h>

namespace oa = geomsrv::archviz::overlayannotations;
namespace layers = geomsrv::archviz::overlaylayers;
namespace annotation = geomsrv::annotation;

namespace {

annotation::Primitive Make (annotation::PrimitiveKind kind, std::vector<annotation::Point3> points,
                            const std::string& text = std::string ())
{
    annotation::Primitive primitive;
    primitive.kind = kind;
    primitive.points = std::move (points);
    primitive.text = text;
    return primitive;
}

} // namespace

TEST (OverlayAnnotations, EveryKindBecomesWhatTheOverlaysDraw)
{
    annotation::Frame frame;
    frame.primitives.push_back (Make (annotation::PrimitiveKind::Point, { { 1, 1, 0 } }, "P"));
    frame.primitives.push_back (Make (annotation::PrimitiveKind::Label, { { 2, 2, 0 } }, "note"));
    annotation::Primitive ring = Make (annotation::PrimitiveKind::Polyline, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 } });
    ring.closed = true;
    frame.primitives.push_back (ring);
    frame.primitives.push_back (Make (annotation::PrimitiveKind::Arrow, { { 0, 0, 0 }, { 3, 0, 0 } }, "flow"));
    frame.primitives.push_back (Make (annotation::PrimitiveKind::Dimension, { { 0, 0, 0 }, { 5, 0, 0 } }));
    frame.primitives.push_back (Make (annotation::PrimitiveKind::Angle, { { 0, 0, 0 }, { 2, 0, 0 }, { 0, 2, 0 } }));
    annotation::Primitive element = Make (annotation::PrimitiveKind::Element, {});
    element.guid = "00000000-0000-0000-0000-000000000001";
    frame.primitives.push_back (element);

    const oa::Built built = oa::BuildLayer (frame);
    const layers::Layer& layer = built.layer;
    EXPECT_EQ (layer.name, oa::kLayerName);
    EXPECT_EQ (layers::Validate (layer), "");
    EXPECT_EQ (built.primitives, 7u);
    EXPECT_EQ (built.drawn, 6u); // the element names a GUID and draws nothing
    ASSERT_EQ (layer.points.size (), 1u);
    ASSERT_EQ (layer.dimensions.size (), 1u);
    // The ring, the arrow's shaft and head, the angle's arc.
    ASSERT_EQ (layer.polylines.size (), 4u);
    EXPECT_TRUE (layer.polylines[0].closed);
    EXPECT_EQ (layer.polylines[2].points.size (), 9u); // left leg, tip, right leg
    EXPECT_DOUBLE_EQ (layer.polylines[2].points[3], 3.0);
    // The point's text, the label, the arrow's text and the angle's degrees.
    ASSERT_EQ (layer.texts.size (), 4u);
    EXPECT_EQ (layer.texts[1].text, "note");
    EXPECT_EQ (layer.texts[3].text, "90.0\xC2\xB0");
}

TEST (OverlayAnnotations, RolesKeepTheirColours)
{
    annotation::Frame frame;
    annotation::Primitive added = Make (annotation::PrimitiveKind::Label, { { 0, 0, 0 } }, "new");
    added.role = annotation::SemanticRole::Add;
    frame.primitives.push_back (added);
    const oa::Built built = oa::BuildLayer (frame);
    ASSERT_EQ (built.layer.texts.size (), 1u);
    EXPECT_EQ (built.layer.texts[0].rgba, 0x189C5CFFu);
}

TEST (OverlayAnnotations, ADimensionKeepsItsSidePlaneAndOffset)
{
    annotation::Frame frame;
    annotation::Primitive dimension = Make (annotation::PrimitiveKind::Dimension, { { 0, 0, 0 }, { 0, 0, 3 } }, "3 m");
    dimension.offset = 0.75;
    dimension.preferredOffsetDirection = annotation::Point3 { 0, -1, 0 };
    dimension.planeNormal = annotation::Point3 { 1, 0, 0 };
    frame.primitives.push_back (dimension);
    const oa::Built built = oa::BuildLayer (frame);
    ASSERT_EQ (built.layer.dimensions.size (), 1u);
    const layers::Dimension& out = built.layer.dimensions[0];
    EXPECT_DOUBLE_EQ (out.offsetMetres, 0.75);
    EXPECT_DOUBLE_EQ (out.direction[1], -1.0);
    EXPECT_DOUBLE_EQ (out.normal[0], 1.0);
    EXPECT_EQ (out.text, "3 m");
}

TEST (OverlayAnnotations, ASlopingArrowStillGetsItsHead)
{
    annotation::Frame frame;
    frame.primitives.push_back (Make (annotation::PrimitiveKind::Arrow, { { 0, 0, 0 }, { 2, 0, 2 } }));
    const oa::Built built = oa::BuildLayer (frame);
    EXPECT_EQ (built.layer.polylines.size (), 2u);
}
