#include "ArchViz/EditLayers.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;
namespace el = geomsrv::archviz::editlayers;
namespace layers = geomsrv::archviz::overlaylayers;

TEST (EditLayers, FormatsDimensionLabels)
{
    EXPECT_EQ (el::FormatValue (ed::Quantity::Length, 4.2), "4.20 m");
    EXPECT_EQ (el::FormatValue (ed::Quantity::Slope, -2.04), "-2.0 %");
    EXPECT_EQ (el::FormatValue (ed::Quantity::Angle, 53.13), "53.1\xC2\xB0");
    EXPECT_EQ (el::FormatValue (ed::Quantity::Elevation, 1.5), "+1.50 m");
}

TEST (EditLayers, EarClippingHandlesConcaveOutlines)
{
    // An L: six corners, four triangles, none outside the L.
    const std::vector<ed::Vec3> l = { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 1, 0 }, { 1, 1, 0 }, { 1, 3, 0 }, { 0, 3, 0 } };
    const auto tris = el::TriangulateXY (l);
    ASSERT_EQ (tris.size (), 12u);
    double area = 0;
    for (size_t t = 0; t < tris.size (); t += 3) {
        const auto a = l[tris[t]], b = l[tris[t + 1]], c = l[tris[t + 2]];
        const double twice = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        EXPECT_GT (twice, 0.0);
        area += twice / 2;
    }
    EXPECT_NEAR (area, 6.0, 1e-12);
}

TEST (EditLayers, DocumentBecomesACommandScopedLayer)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 } }, false);
    ed::AddExtrusion (doc, { { 10, 0, 0 }, { 12, 0, 0 }, { 12, 2, 0 }, { 10, 2, 0 } }, 3.0);
    const auto layer = el::Build (doc, {});
    EXPECT_EQ (layer.name, el::kLayer);
    EXPECT_EQ (layer.lifetime, layers::LayerLifetime::Command);
    // Polyline + bottom ring + top ring + four verticals.
    EXPECT_EQ (layer.polylines.size (), 7u);
    ASSERT_EQ (layer.meshes.size (), 1u);
    EXPECT_EQ (layer.meshes[0].indices.size (), 3u * (2 * 2 + 2 * 4));
    ASSERT_EQ (layer.points.size (), 1u);
    EXPECT_EQ (layer.points[0].points.size (), 3u * 7);
}

TEST (EditLayers, SelectionSnapDimensionsAndGizmoAreDrawn)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 10 }, { 4, 0, 10.2 } }, false);
    const ed::Ref seg { e, ed::Part::Segment, ed::Find (doc, e)->segments[0].id };
    ed::Selection sel;
    sel.Set ({ seg });
    ed::SnapResult snap;
    snap.snapped = true;
    snap.point = { 2, 0, 10 };
    snap.hint = "Midpoint";
    snap.guides.push_back ({ { 0, 0, 10 }, { 2, 0, 10 }, ed::SnapKind::Axis });
    el::Feedback f;
    f.selection = &sel;
    f.snap = &snap;
    f.dimensions = ed::DimensionsFor (doc, seg);
    f.gizmo.shown = true;
    f.gizmo.size = 1.0;
    f.gizmo.active = 1;
    const auto layer = el::Build (doc, f);
    ASSERT_EQ (layer.dimensions.size (), 1u); // the length; slope and angle are labels
    EXPECT_EQ (layer.dimensions[0].text, "4.00 m");
    bool slope = false, hint = false;
    for (const auto& t : layer.texts) {
        slope |= t.text == "5.0 %";
        hint |= t.text == "Midpoint";
    }
    EXPECT_TRUE (slope);
    EXPECT_TRUE (hint);
    size_t dashed = 0, arrows = 0, highlighted = 0;
    for (const auto& p : layer.polylines) {
        dashed += p.dashMetres.empty () ? 0 : 1;
        arrows += p.endArrow == layers::Terminator::Arrow ? 1 : 0;
        highlighted += p.rgba == el::Style {}.selected ? 1 : 0;
    }
    EXPECT_EQ (dashed, 1u);      // the guide
    EXPECT_EQ (highlighted, 1u); // the selected segment
    EXPECT_EQ (arrows, 1u + 3u); // the slope arrow and three axis handles
    bool active = false;
    for (const auto& p : layer.polylines)
        active |= p.rgba == el::Style {}.activeHandle && p.widthPixels > 3.0f;
    EXPECT_TRUE (active);
}
