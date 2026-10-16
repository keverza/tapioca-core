#include "ArchViz/EditLayers.hpp"
#include "Edit/EditPathEdit.hpp"
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

TEST (EditLayers, PathsDrawSurfaceGradesCrossfallAndFixedPoints)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 100.0 }, { 40, 0, 102.0 } }, 2.0);
    std::string error;
    ASSERT_TRUE (ed::FixElevationAt (doc, path, 40.0, 102.0, "door", error)) << error;
    const auto layer = el::Build (doc, {});
    ASSERT_EQ (layer.meshes.size (), 1u);
    EXPECT_FALSE (layer.meshes[0].indices.empty ());
    bool grade = false, fixed = false, crossfall = false;
    for (const auto& t : layer.texts) {
        grade |= t.text == "5.0 %";
        fixed |= t.text == "+102.00 m fixed";
        crossfall |= t.text == "2.0 %" && t.rgba == el::Style {}.fall;
    }
    EXPECT_TRUE (grade);
    EXPECT_TRUE (fixed);
    EXPECT_TRUE (crossfall);
    // One downhill grade arrow (pointing back towards the start) and two crown arrows.
    std::vector<const layers::Polyline*> arrows;
    for (const auto& p : layer.polylines)
        if (p.endArrow == layers::Terminator::Arrow)
            arrows.push_back (&p);
    ASSERT_EQ (arrows.size (), 3u);
    const auto* downhill = arrows[0];
    EXPECT_LT (downhill->points[3], downhill->points[0]); // x decreases: water runs to the start
    bool red = false;
    for (const auto& p : layer.points)
        red |= p.rgba == el::Style {}.fixedBreak;
    EXPECT_TRUE (red);
}

TEST (EditLayers, SelectedPathPartsAreHighlighted)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, 2.0);
    ed::Selection sel;
    sel.Set ({ { path, ed::Part::Range, ed::FindPath (doc, path)->sections[0].id } });
    el::Feedback f;
    f.selection = &sel;
    const auto layer = el::Build (doc, f);
    size_t highlighted = 0;
    for (const auto& p : layer.polylines)
        highlighted += p.rgba == el::Style {}.selected ? 1 : 0;
    EXPECT_EQ (highlighted, 2u); // both edges of the range
    f.dimensions = ed::DimensionsFor (doc, sel.Items ()[0]);
    const auto withDims = el::Build (doc, f);
    bool width = false;
    for (const auto& t : withDims.texts)
        width |= t.text == "2.00 m";
    EXPECT_TRUE (width);
}

TEST (EditLayers, LiveDimensionsSayOnlyWhatIsWorthSaying)
{
    ed::Document doc;
    const ed::Id flat = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 3, 1 } }, false);
    const auto& e = *ed::Find (doc, flat);
    const auto first = el::LiveDimensions (doc, { flat, ed::Part::Segment, e.segments[0].id });
    ASSERT_EQ (first.size (), 2u); // length and angle: a flat slope says nothing
    EXPECT_EQ (first[0].text, "4.00 m");
    EXPECT_TRUE (first[0].line);
    EXPECT_EQ (first[1].quantity, ed::Quantity::Angle);
    const auto second = el::LiveDimensions (doc, { flat, ed::Part::Segment, e.segments[1].id });
    ASSERT_EQ (second.size (), 3u);
    EXPECT_EQ (second[1].text, "33.3 %");
    const auto vertex = el::LiveDimensions (doc, { flat, ed::Part::Vertex, e.vertices[2].id });
    ASSERT_EQ (vertex.size (), 1u); // its height; plan coordinates are the tracker's
    EXPECT_EQ (vertex[0].text, "Z +1.00 m");
}

TEST (EditLayers, LabelsAreLaidOutClearOfWhatIsTaken)
{
    std::vector<el::Box> taken { { 90, 90, 110, 110 } }; // the gizmo
    const el::Box a = el::PlaceLabel (100, 100, 40, 16, 0, -1, 0, taken);
    EXPECT_FALSE (el::Overlaps (a, taken[0]));
    EXPECT_LT (a.y1, 100.0f); // pushed out the way it was asked to go
    const el::Box b = el::PlaceLabel (100, 100, 40, 16, 0, -1, 0, taken);
    EXPECT_FALSE (el::Overlaps (b, taken[0]));
    EXPECT_FALSE (el::Overlaps (b, a));
    EXPECT_EQ (taken.size (), 3u);
}

TEST (EditLayers, NothingIsDrawnWhite)
{
    // Archicad's plan is white: every stroke and text colour must stand off it.
    const el::Style style;
    const auto light = [] (uint32_t rgba) {
        const int r = int (rgba >> 24), g = int ((rgba >> 16) & 0xFF), b = int ((rgba >> 8) & 0xFF);
        return r + g + b > 3 * 225;
    };
    for (uint32_t c :
         { style.geometry, style.selected, style.hover, style.guide, style.snap, style.activeHandle, style.text,
           style.dimension, style.pathEdge, style.controlPolygon, style.gradeBreak, style.fixedBreak, style.fall,
           style.junctionBreak, style.axis[0], style.axis[1], style.axis[2] })
        EXPECT_FALSE (light (c)) << std::hex << c;
    EXPECT_TRUE (light (style.paper)); // only the pills under text are light
}

TEST (EditLayers, LiveLengthSaysWhichEndItKeeps)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    const ed::Ref seg { e, ed::Part::Segment, ed::Find (doc, e)->segments[0].id };
    const auto start = el::LiveDimensions (doc, seg);
    ASSERT_FALSE (start.empty ());
    ASSERT_TRUE (start[0].ends);
    EXPECT_NEAR (start[0].fixed.x, 0.0, 1e-12);
    EXPECT_NEAR (start[0].moving.x, 4.0, 1e-12);
    const auto end = el::LiveDimensions (doc, seg, true);
    EXPECT_NEAR (end[0].fixed.x, 4.0, 1e-12);
    EXPECT_EQ (end[1].text.substr (0, 5), "180.0"); // the angle reads from the kept end
}

TEST (EditLayers, ALabelWithNoFreePlaceIsLeftOut)
{
    // Everything round the anchor is taken: the label is refused, not stacked on another.
    std::vector<el::Box> taken { { -1000, -1000, 1000, 1000 } };
    bool found = true;
    el::PlaceLabel (0, 0, 40, 16, 0, -1, 0, taken, &found);
    EXPECT_FALSE (found);
    EXPECT_EQ (taken.size (), 1u);
    el::PlaceLabel (5000, 5000, 40, 16, 0, -1, 0, taken, &found);
    EXPECT_TRUE (found);
    EXPECT_EQ (taken.size (), 2u);
}

TEST (EditLayers, PathLabelsGoToTheViewWhenItPlacesThem)
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 1 } }, 1.5);
    const auto labels = el::PathLabels (doc);
    // Two break heights, one grade, one crossfall, and its width for the view only; the grade
    // stands off to the left of the path, the crossfall towards its falling edge.
    ASSERT_EQ (labels.size (), 5u);
    size_t percents = 0, lines = 0;
    for (const auto& l : labels) {
        EXPECT_NEAR (std::hypot (l.side.x, l.side.y), 1.0, 1e-9);
        EXPECT_TRUE (l.typed) << l.text; // every one can be typed over
        percents += l.text.find ('%') != std::string::npos;
        lines += l.line;
    }
    EXPECT_EQ (percents, 2u);
    EXPECT_EQ (lines, 1u);
    el::Feedback inView;
    inView.labelsInView = true;
    EXPECT_TRUE (el::Build (doc, inView).texts.empty ());
    EXPECT_EQ (el::Build (doc, {}).texts.size (), labels.size () - lines);
}

TEST (EditLayers, TurnedLabelsOverlapAsTheyStand)
{
    // Two 60 x 14 labels reading along the diagonal, 20 px apart across it: their level
    // envelopes overlap, the labels do not. Turned across each other, they do.
    const float k = 0.70710678f;
    const el::Box a { -30, -7, 30, 7, k, k };
    el::Box b { -30 - 20 * k, -7 + 20 * k, 30 - 20 * k, 7 + 20 * k, k, k };
    EXPECT_FALSE (el::Overlaps (a, b));
    b.ux = k, b.uy = -k;
    EXPECT_TRUE (el::Overlaps (a, b));
    // Placed along a direction, a label keeps it.
    std::vector<el::Box> taken;
    const el::Box c = el::PlaceAlong (0, 0, 60, 14, k, k, -k, k, 10, taken);
    EXPECT_FLOAT_EQ (c.ux, k);
    EXPECT_FLOAT_EQ (c.uy, k);
}

TEST (EditLayers, LabelsNeverReadUpsideDown)
{
    float x = -1.0f, y = 0.2f;
    el::Upright (x, y);
    EXPECT_GT (x, 0.0f); // right to left becomes left to right
    x = 0.0f, y = 1.0f;  // straight down the screen...
    el::Upright (x, y);
    EXPECT_FLOAT_EQ (y, -1.0f); // ...reads bottom to top
    x = 0.0f, y = 0.0f;
    el::Upright (x, y);
    EXPECT_FLOAT_EQ (x, 1.0f); // no direction: level
}

TEST (EditLayers, PathLabelsReadAlongWhatTheyMeasure)
{
    // A path running north: its heights and grade read along it, its crossfall and width
    // across it.
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 0, 20, 1 } }, 1.5);
    for (const auto& l : el::PathLabels (doc)) {
        const bool crossfall = l.rgba == el::Style {}.fall || l.line; // and the width across it
        EXPECT_NEAR (std::abs (l.along.y), crossfall ? 0.0 : 1.0, 1e-9) << l.text;
        EXPECT_NEAR (std::abs (l.along.x), crossfall ? 1.0 : 0.0, 1e-9) << l.text;
    }
}

TEST (EditLayers, PathDimensionsMeasureWidthsAndRadii)
{
    // A 2 m path bending round a 6 m arc, kerbs rounded 0.5 m: its width across it, the
    // bend's radius out from the arc's centre, each kerb fillet's from its own.
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 }, { 20, 20, 0 } }, 2.0);
    std::string error;
    ASSERT_TRUE (ed::SetPathRadius (doc, p, ed::FindPath (doc, p)->points[1].id, 6.0, error)) << error;
    ASSERT_TRUE (ed::SetPathEdgeRadius (doc, p, 0.5, error)) << error;
    size_t widths = 0, bends = 0, kerbs = 0;
    for (const auto& l : el::PathLabels (doc)) {
        if (!l.line)
            continue;
        const double length = std::hypot (l.to.x - l.from.x, l.to.y - l.from.y);
        EXPECT_NEAR (length, l.extent, 1e-6) << l.text; // the line is what it measures
        if (!l.radial) {
            ++widths;
            EXPECT_EQ (l.text, "2.00 m");
            EXPECT_EQ (l.value.quantity, ed::Quantity::Width);
        }
        else if (l.kerbPath != ed::kNoId) {
            ++kerbs;
            EXPECT_EQ (l.text, "R 0.50 m");
        }
        else {
            ++bends;
            EXPECT_EQ (l.text, "R 6.00 m");
            EXPECT_NEAR (l.from.x, 14.0, 1e-6); // the arc's centre
            EXPECT_NEAR (l.from.y, 6.0, 1e-6);
        }
    }
    EXPECT_EQ (widths, 1u);
    EXPECT_EQ (bends, 1u);
    EXPECT_EQ (kerbs, 4u); // the four end corners
}
