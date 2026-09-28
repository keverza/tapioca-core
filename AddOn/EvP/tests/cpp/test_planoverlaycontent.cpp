// ArchViz/PlanOverlayContent: the floor-plan overlay's segments from the plan's own rings.

#include "ArchViz/PlanOverlayContent.hpp"

#include "ArchViz/PlanAnchorRibbon.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace pc = geomsrv::archviz::plancontent;

namespace {

// A segment's end points as the layer reconstructs them: origin + hi + lo.
double X0 (const pc::Content& c, const pc::Segment& s)
{
    return c.originX + double (s.x0) + double (s.x0Lo);
}
double Y0 (const pc::Content& c, const pc::Segment& s)
{
    return c.originY + double (s.y0) + double (s.y0Lo);
}
double X1 (const pc::Content& c, const pc::Segment& s)
{
    return c.originX + double (s.x1) + double (s.x1Lo);
}
double Y1 (const pc::Content& c, const pc::Segment& s)
{
    return c.originY + double (s.y1) + double (s.y1Lo);
}

constexpr double kPi = 3.14159265358979323846;

} // namespace

TEST (PlanOverlayContent, AStraightRingClosesIntoItsOwnSegments)
{
    const std::vector<std::vector<double>> rings = { { 0.0, 0.0, 4.0, 0.0, 4.0, 0.3, 0.0, 0.3 } };
    const pc::Content content = pc::BuildContent (rings, {}, 1.0, 0.05);
    ASSERT_EQ (content.rings, 1u);
    ASSERT_EQ (content.segments.size (), 4u);
    EXPECT_DOUBLE_EQ (content.originX, 2.0);
    EXPECT_DOUBLE_EQ (content.originY, 0.15);
    // The last segment closes back to the first point.
    const pc::Segment& last = content.segments.back ();
    EXPECT_NEAR (X1 (content, last), 0.0, 1e-12);
    EXPECT_NEAR (Y1 (content, last), 0.0, 1e-12);
}

// ⚠️ A GEOREFERENCED PROJECT KEEPS ITS MICROMETRES. Hundreds of kilometres out a float
// keeps centimetres; relative to the centre, as a float pair, the reconstruction is
// double's.
TEST (PlanOverlayContent, FarFromTheOriginTheSegmentsKeepDoublePrecision)
{
    const double east = 512345.678, north = 6543210.123;
    const std::vector<std::vector<double>> rings = { { east, north, east + 0.001, north, east + 0.001, north + 0.001,
                                                       east, north + 0.001 } };
    const pc::Content content = pc::BuildContent (rings, {}, 1.0, 0.05);
    ASSERT_EQ (content.segments.size (), 4u);
    const pc::Segment& first = content.segments.front ();
    EXPECT_NEAR (X1 (content, first) - X0 (content, first), 0.001, 1e-9);
    EXPECT_NEAR (X0 (content, first), east, 1e-9);
    EXPECT_NEAR (Y0 (content, first), north, 1e-9);
}

// ⚠️ AND A LARGE SITE KEEPS THEM AT ITS FAR END. A kilometre from the content's centre
// one float has ~60 micrometres -- six pixels at the closest plan zoom. The pair keeps
// the point to well under a nanometre.
TEST (PlanOverlayContent, AKilometreFromTheCentreTheSplitStillHoldsDoublePrecision)
{
    const std::vector<std::vector<double>> rings = { { -1000.0, 0.0, -999.9999876, 0.0000123 },
                                                     { 1000.0, 0.0, 1000.1, 0.1 } };
    const pc::Content content = pc::BuildContent (rings, {}, 1.0, 0.05);
    ASSERT_FALSE (content.segments.empty ());
    const pc::Segment& first = content.segments.front ();
    EXPECT_NEAR (X0 (content, first), -1000.0, 1e-9);
    EXPECT_NEAR (X1 (content, first), -999.9999876, 1e-9);
    EXPECT_NEAR (Y1 (content, first), 0.0000123, 1e-9);
    // One float alone could not have said it.
    EXPECT_GT (std::fabs (double (first.x1) + content.originX - (-999.9999876)), 1e-7);
}

TEST (PlanOverlayContent, AnArcedEdgeIsTessellatedAndStaysOnItsCircle)
{
    // A half disc: the straight diameter, then a 180-degree arc back.
    const std::vector<std::vector<double>> rings = { { -1.0, 0.0, 1.0, 0.0 } };
    const std::vector<std::vector<double>> arcs = { { 0.0, kPi } };
    const pc::Content content = pc::BuildContent (rings, arcs, 1.0, 0.05);
    ASSERT_GT (content.segments.size (), 10u);
    bool above = false;
    for (const pc::Segment& s : content.segments) {
        const double x = X0 (content, s);
        const double y = Y0 (content, s);
        // Every vertex is on the diameter (y = 0) or on the unit circle.
        EXPECT_TRUE (std::fabs (y) < 1e-12 || std::fabs (std::hypot (x, y) - 1.0) < 1e-12) << x << "," << y;
        above = above || y > 0.5;
    }
    // From (1,0) back to (-1,0) counter-clockwise is the upper half.
    EXPECT_TRUE (above);
}

// ⚠️ ONE READING OF AN ARC, NOT TWO. The double tessellation must give TessellateEdge's
// points -- the same centre, the same direction, the same count -- for either sign and
// either reading of the sign, or the plan overlay and the anchor ribbon would bulge
// differently on the same wall.
TEST (PlanOverlayContent, TheDoubleArcIsTessellateEdgesArc)
{
    struct Case {
        double x0, y0, x1, y1, angle, sign;
    };
    const Case cases[] = {
        { 1.0, 0.0, 0.0, 1.0, kPi / 2.0, 1.0 }, { 1.0, 0.0, 0.0, 1.0, kPi / 2.0, -1.0 },
        { 3.0, 2.0, 5.0, 2.5, -0.7, 1.0 },      { -2.0, 1.0, 2.0, 1.0, kPi, 1.0 },
        { 0.0, 0.0, 10.0, 0.0, 0.3, 1.0 },      { 0.0, 0.0, 0.0, 0.0, 1.0, 1.0 },
        { 0.0, 0.0, 1.0, 1.0, 0.0, 1.0 },
    };
    for (const Case& c : cases) {
        std::vector<float> single;
        geomsrv::archviz::TessellateEdge (float (c.x0), float (c.y0), float (c.x1), float (c.y1), float (c.angle),
                                          float (c.sign), 0.05f, single);
        std::vector<double> twin;
        pc::TessellateEdgeDouble (c.x0, c.y0, c.x1, c.y1, c.angle, c.sign, 0.05, twin);
        ASSERT_EQ (single.size (), twin.size ()) << c.x0 << "," << c.y0 << " -> " << c.x1 << "," << c.y1;
        // A different reading is off by the arc's bulge -- decimetres here; float
        // rounding on a 33 m radius is micrometres.
        for (size_t i = 0; i < twin.size (); ++i)
            EXPECT_NEAR (double (single[i]), twin[i], 1e-4);
    }
}

TEST (PlanOverlayContent, ARepeatedPointDrawsNoSegment)
{
    const std::vector<std::vector<double>> rings = { { 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0 } };
    const pc::Content content = pc::BuildContent (rings, {}, 1.0, 0.05);
    EXPECT_EQ (content.segments.size (), 3u);
    for (const pc::Segment& s : content.segments)
        EXPECT_GT (std::hypot (X1 (content, s) - X0 (content, s), Y1 (content, s) - Y0 (content, s)), 0.5);
}

TEST (PlanOverlayContent, NothingInIsNothingOut)
{
    EXPECT_TRUE (pc::BuildContent ({}, {}, 1.0, 0.05).segments.empty ());
    EXPECT_TRUE (pc::BuildContent ({ { 1.0, 2.0 } }, {}, 1.0, 0.05).segments.empty ());
    EXPECT_EQ (pc::BuildContent ({ { 1.0, 2.0 } }, {}, 1.0, 0.05).rings, 0u);
}

// ---- the frame's projection ----------------------------------------------------

namespace {

// A plan transform: `scale` pixels per metre, the view turned by `degrees`, y down on
// screen, and the model point (anchorX, anchorY) at the centre of a width x height buffer.
pc::PixelTransform PlanTransform (double scale, double degrees, double anchorX, double anchorY, uint32_t width,
                                  uint32_t height)
{
    const double a = degrees * kPi / 180.0;
    pc::PixelTransform t;
    t.xx = scale * std::cos (a);
    t.xy = -scale * std::sin (a);
    t.yx = -scale * std::sin (a);
    t.yy = -scale * std::cos (a);
    t.ox = double (width) * 0.5 - (t.xx * anchorX + t.xy * anchorY);
    t.oy = double (height) * 0.5 - (t.yx * anchorX + t.yy * anchorY);
    return t;
}

// How far the shader's float arithmetic lands from the double projection, in pixels,
// for a model point given relative to the content origin.
double ShaderError (const pc::PixelTransform& t, const pc::ViewConstants& c, double originX, double originY,
                    double relX, double relY)
{
    float hiX, loX, hiY, loY;
    pc::Split (relX, hiX, loX);
    pc::Split (relY, hiY, loY);
    float px = 0.0f, py = 0.0f;
    pc::ShaderPixel (c, hiX, hiY, loX, loY, px, py);
    const double mx = originX + relX, my = originY + relY;
    const double ex = t.xx * mx + t.xy * my + t.ox;
    const double ey = t.yx * mx + t.yy * my + t.oy;
    return std::hypot (double (px) - ex, double (py) - ey);
}

} // namespace

TEST (PlanOverlayProjection, AtWorkingZoomAGeoreferencedPlanLandsOnItsPixels)
{
    // 150 px/m, a project half a thousand kilometres out, the view on its corner.
    const double originX = 512345.678, originY = 6543210.123;
    const pc::PixelTransform t = PlanTransform (150.0, 0.0, originX + 18.0, originY - 11.0, 3014, 1854);
    pc::ViewConstants c;
    ASSERT_TRUE (pc::MakeViewConstants (t, originX, originY, 3014, 1854, c));
    for (double dx = -9.0; dx <= 9.0; dx += 1.7)
        for (double dy = -6.0; dy <= 6.0; dy += 1.3)
            EXPECT_LT (ShaderError (t, c, originX, originY, 18.0 + dx, -11.0 + dy), 1e-3);
}

// ⚠️ EXTREME CLOSE ZOOM IS PERMANENT IN THE REGRESSION MATRIX (§14), SO IT IS HELD HERE.
// A millimetre across a thousand pixels, the view turned, a kilometre from the content's
// centre: one float per coordinate is ~60 micrometres there -- sixty pixels. The split
// halves, differenced against a split anchor, keep every point on screen within a
// hundredth of a pixel.
TEST (PlanOverlayProjection, AtExtremeCloseZoomAKilometreOutStaysWithinAHundredthOfAPixel)
{
    const double originX = 0.0, originY = 0.0;
    const double anchorX = 1000.0001234, anchorY = -999.9998765;
    const pc::PixelTransform t = PlanTransform (1.0e6, 30.0, anchorX, anchorY, 3014, 1854);
    pc::ViewConstants c;
    ASSERT_TRUE (pc::MakeViewConstants (t, originX, originY, 3014, 1854, c));
    double worst = 0.0;
    for (double dx = -1.4e-3; dx <= 1.4e-3; dx += 0.37e-3)
        for (double dy = -0.9e-3; dy <= 0.9e-3; dy += 0.29e-3)
            worst = std::max (worst, ShaderError (t, c, originX, originY, anchorX + dx, anchorY + dy));
    EXPECT_LT (worst, 0.01);

    // And one float alone could not have done it: the same point, its low half dropped.
    float px = 0.0f, py = 0.0f;
    float hiX, loX, hiY, loY;
    pc::Split (anchorX + 0.5e-3, hiX, loX);
    pc::Split (anchorY, hiY, loY);
    pc::ShaderPixel (c, hiX, hiY, 0.0f, 0.0f, px, py);
    const double ex = t.xx * (anchorX + 0.5e-3) + t.xy * anchorY + t.ox;
    const double ey = t.yx * (anchorX + 0.5e-3) + t.yy * anchorY + t.oy;
    EXPECT_GT (std::hypot (double (px) - ex, double (py) - ey), 1.0);
}

TEST (PlanOverlayProjection, TheAnchorIsTheBuffersCentre)
{
    const pc::PixelTransform t = PlanTransform (240.0, 12.5, 7.25, -3.5, 2000, 1000);
    pc::ViewConstants c;
    ASSERT_TRUE (pc::MakeViewConstants (t, 5.0, -1.0, 2000, 1000, c));
    EXPECT_NEAR (c.screen[0], 1000.0, 1e-3);
    EXPECT_NEAR (c.screen[1], 500.0, 1e-3);
    EXPECT_NEAR (double (c.view[0]) + double (c.view[2]), 7.25 - 5.0, 1e-12);
    EXPECT_NEAR (double (c.view[1]) + double (c.view[3]), -3.5 + 1.0, 1e-12);
    EXPECT_FLOAT_EQ (c.screen[2], 2.0f / 2000.0f);
    EXPECT_FLOAT_EQ (c.screen[3], 2.0f / 1000.0f);
}

TEST (PlanOverlayProjection, ADegenerateTransformHasNoAnchor)
{
    pc::PixelTransform flat;
    flat.xx = 100.0;
    flat.xy = 100.0;
    flat.yx = 1.0;
    flat.yy = 1.0;
    pc::ViewConstants c;
    c.screen[0] = 42.0f;
    EXPECT_FALSE (pc::MakeViewConstants (flat, 0.0, 0.0, 100, 100, c));
    EXPECT_FLOAT_EQ (c.screen[0], 42.0f); // untouched
    EXPECT_FALSE (pc::MakeViewConstants (PlanTransform (100.0, 0.0, 0.0, 0.0, 100, 100), 0.0, 0.0, 0, 100, c));
}
