// ArchViz/PlanFrameRegistration: the pixel ground truth of the floor-plan frame
// record. Every frame here is rendered from one analytic scene -- antialiased line
// segments on paper -- so a pan, a zoom or a sub-sample shift has an exact answer.

#include "ArchViz/PlanFrameRegistration.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pf = geomsrv::archviz::planframes;

namespace {

constexpr uint32_t kWidth = 512;
constexpr uint32_t kHeight = 384;

struct Segment {
    double x0, y0, x1, y1;
    // The box within which the stroke leaves any ink, so a pixel far from it
    // costs one comparison rather than a distance.
    double left () const
    {
        return std::min (x0, x1) - 3.0;
    }
    double right () const
    {
        return std::max (x0, x1) + 3.0;
    }
    double top () const
    {
        return std::min (y0, y1) - 3.0;
    }
    double bottom () const
    {
        return std::max (y0, y1) + 3.0;
    }
};

// A deterministic "plan": walls (long axis-aligned strokes), some diagonals and
// short marks, spread over an area larger than the frame so a pan or a zoom
// out always finds drawing.
std::vector<Segment> Scene (bool horizontalOnly = false)
{
    std::vector<Segment> segments;
    uint32_t state = 12345u;
    auto next = [&state] () {
        state = state * 1664525u + 1013904223u;
        return double ((state >> 8) & 0xFFFFu) / 65535.0;
    };
    for (int i = 0; i < 160; ++i) {
        const double x = -200.0 + next () * 900.0;
        const double y = -200.0 + next () * 800.0;
        const double length = 20.0 + next () * 160.0;
        const int kind = horizontalOnly ? 0 : int (next () * 4.0);
        if (kind == 0)
            segments.push_back ({ x, y, x + length, y });
        else if (kind == 1)
            segments.push_back ({ x, y, x, y + length });
        else if (kind == 2)
            segments.push_back ({ x, y, x + length * 0.7, y + length * 0.5 });
        else
            segments.push_back ({ x, y, x + 6.0, y - 9.0 });
    }
    return segments;
}

double Distance (const Segment& s, double x, double y)
{
    const double dx = s.x1 - s.x0;
    const double dy = s.y1 - s.y0;
    const double lengthSq = dx * dx + dy * dy;
    double t = lengthSq > 0.0 ? ((x - s.x0) * dx + (y - s.y0) * dy) / lengthSq : 0.0;
    t = std::max (0.0, std::min (1.0, t));
    return std::hypot (x - (s.x0 + t * dx), y - (s.y0 + t * dy));
}

// The frame whose sample (x, y) shows the scene at (a*x - b*y + tx, b*x + a*y + ty):
// the scene coordinates of the frame are a similarity of its pixel coordinates.
std::vector<uint8_t> Render (const std::vector<Segment>& scene, double a, double b, double tx, double ty)
{
    std::vector<uint8_t> pixels (size_t (kWidth) * kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const double sx = a * double (x) - b * double (y) + tx;
            const double sy = b * double (x) + a * double (y) + ty;
            double ink = 0.0;
            for (const Segment& segment : scene) {
                if (sx < segment.left () || sx > segment.right () || sy < segment.top () || sy > segment.bottom ())
                    continue;
                const double d = Distance (segment, sx, sy);
                if (d < 3.0)
                    ink = std::max (ink, std::exp (-d * d / (2.0 * 0.8 * 0.8)));
            }
            pixels[size_t (y) * kWidth + x] = uint8_t (std::lround (255.0 - 200.0 * ink));
        }
    }
    return pixels;
}

pf::GreyImage View (const std::vector<uint8_t>& pixels)
{
    pf::GreyImage image;
    image.pixels = pixels.data ();
    image.width = kWidth;
    image.height = kHeight;
    image.stride = kWidth;
    return image;
}

// The motion MeasureFrameMotion must report between two renders: the current
// frame's sample p shows the scene at S_c(p), which the previous frame showed at
// S_p^-1 (S_c (p)).
void Expected (double pa, double pb, double ptx, double pty, double ca, double cb, double ctx, double cty, double x,
               double y, double& ex, double& ey)
{
    const double sx = ca * x - cb * y + ctx;
    const double sy = cb * x + ca * y + cty;
    const double det = pa * pa + pb * pb;
    const double rx = sx - ptx;
    const double ry = sy - pty;
    ex = (pa * rx + pb * ry) / det;
    ey = (-pb * rx + pa * ry) / det;
}

void ExpectMotion (const pf::FrameMotion& motion, double pa, double pb, double ptx, double pty, double ca, double cb,
                   double ctx, double cty, double tolerance)
{
    ASSERT_TRUE (motion.valid) << motion.why << " (textured " << motion.patchesTextured << ", used "
                               << motion.patchesUsed << ")";
    // ⚠️ INSIDE THE PATCH GRID, WHERE THE INSTRUMENT SCORES. A corner of the frame
    // is an extrapolation of the fit, and the diagnostic never reads one.
    const double points[5][2] = {
        { 256.0, 192.0 }, { 128.0, 96.0 }, { 384.0, 96.0 }, { 128.0, 288.0 }, { 384.0, 288.0 }
    };
    for (const auto& p : points) {
        double ex = 0.0, ey = 0.0;
        Expected (pa, pb, ptx, pty, ca, cb, ctx, cty, p[0], p[1], ex, ey);
        double dx = 0.0, dy = 0.0;
        pf::MotionAt (motion, p[0], p[1], dx, dy);
        EXPECT_NEAR (p[0] + dx, ex, tolerance) << "at " << p[0] << "," << p[1];
        EXPECT_NEAR (p[1] + dy, ey, tolerance) << "at " << p[0] << "," << p[1];
    }
}

} // namespace

TEST (PlanFrameRegistration, AStillFrameIsStill)
{
    const std::vector<Segment> scene = Scene ();
    const std::vector<uint8_t> frame = Render (scene, 1.0, 0.0, 0.0, 0.0);
    const pf::FrameMotion motion = pf::MeasureFrameMotion (View (frame), View (frame));
    ASSERT_TRUE (motion.valid) << motion.why;
    EXPECT_NEAR (motion.a, 1.0, 1e-9);
    EXPECT_NEAR (motion.b, 0.0, 1e-9);
    EXPECT_NEAR (motion.offsetX, 0.0, 1e-9);
    EXPECT_NEAR (motion.offsetY, 0.0, 1e-9);
}

TEST (PlanFrameRegistration, IntegerPansAreExact)
{
    const std::vector<Segment> scene = Scene ();
    const std::vector<uint8_t> previous = Render (scene, 1.0, 0.0, 0.0, 0.0);
    for (const auto& pan :
         { std::pair<double, double> { 7.0, -3.0 }, { -25.0, 11.0 }, { 40.0, 38.0 }, { -60.0, 0.0 } }) {
        const std::vector<uint8_t> current = Render (scene, 1.0, 0.0, pan.first, pan.second);
        const pf::FrameMotion motion = pf::MeasureFrameMotion (View (previous), View (current));
        ExpectMotion (motion, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, pan.first, pan.second, 0.05);
    }
}

// ⚠️ A SUB-SAMPLE PAN IS THE CASE THAT DECIDES "WITHIN A PIXEL". The kept frame is
// a 2x2 average of the back buffer, so one sample is two physical pixels; the
// vertex fit has to recover fractions or the instrument cannot tell 0.5 px from 1.
TEST (PlanFrameRegistration, FractionalPansAreRecoveredToATenthOfASample)
{
    const std::vector<Segment> scene = Scene ();
    const std::vector<uint8_t> previous = Render (scene, 1.0, 0.0, 0.0, 0.0);
    for (const auto& pan : { std::pair<double, double> { 0.25, 0.0 }, { 3.5, -1.25 }, { -12.75, 5.4 } }) {
        const std::vector<uint8_t> current = Render (scene, 1.0, 0.0, pan.first, pan.second);
        const pf::FrameMotion motion = pf::MeasureFrameMotion (View (previous), View (current));
        ExpectMotion (motion, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, pan.first, pan.second, 0.1);
    }
}

// A zoom step is not a translation anywhere but its centre; the warped
// re-measurement is what makes it as exact as a pan.
TEST (PlanFrameRegistration, ZoomStepsAboutAPointAreMeasured)
{
    const std::vector<Segment> scene = Scene ();
    const std::vector<uint8_t> previous = Render (scene, 1.0, 0.0, 0.0, 0.0);
    for (const double scale : { 1.05, 0.95, 1.12, 0.9 }) {
        SCOPED_TRACE (testing::Message () << "scale " << scale);
        // Zoom about (300, 170) of the frame: scene = c + (p - c) * scale.
        const double cx = 300.0, cy = 170.0;
        const double tx = cx - scale * cx;
        const double ty = cy - scale * cy;
        const std::vector<uint8_t> current = Render (scene, scale, 0.0, tx, ty);
        const pf::FrameMotion motion = pf::MeasureFrameMotion (View (previous), View (current));
        ExpectMotion (motion, 1.0, 0.0, 0.0, 0.0, scale, 0.0, tx, ty, 0.2);
    }
}

TEST (PlanFrameRegistration, AZoomWhilePanningIsOneSimilarity)
{
    const std::vector<Segment> scene = Scene ();
    const std::vector<uint8_t> previous = Render (scene, 1.02, 0.0, 5.0, -4.0);
    const std::vector<uint8_t> current = Render (scene, 0.97, 0.0, 18.0, 9.5);
    const pf::FrameMotion motion = pf::MeasureFrameMotion (View (previous), View (current));
    ExpectMotion (motion, 1.02, 0.0, 5.0, -4.0, 0.97, 0.0, 18.0, 9.5, 0.2);
}

TEST (PlanFrameRegistration, ASmallRotationIsCarriedByB)
{
    const std::vector<Segment> scene = Scene ();
    const std::vector<uint8_t> previous = Render (scene, 1.0, 0.0, 0.0, 0.0);
    const double angle = 0.01;
    const std::vector<uint8_t> current = Render (scene, std::cos (angle), std::sin (angle), 4.0, 2.0);
    const pf::FrameMotion motion = pf::MeasureFrameMotion (View (previous), View (current));
    ExpectMotion (motion, 1.0, 0.0, 0.0, 0.0, std::cos (angle), std::sin (angle), 4.0, 2.0, 0.15);
}

TEST (PlanFrameRegistration, BlankPaperIsRefusedNotMatched)
{
    const std::vector<uint8_t> paper (size_t (kWidth) * kHeight, 255);
    const pf::FrameMotion motion = pf::MeasureFrameMotion (View (paper), View (paper));
    EXPECT_FALSE (motion.valid);
}

// ⚠️ THE APERTURE PROBLEM. Horizontal strokes locate a vertical shift and say
// nothing about a horizontal one; the answer must be "unmeasured", never a
// confident zero.
TEST (PlanFrameRegistration, OnlyHorizontalStrokesCannotClaimAHorizontalShift)
{
    const std::vector<Segment> scene = Scene (/*horizontalOnly*/ true);
    const std::vector<uint8_t> previous = Render (scene, 1.0, 0.0, 0.0, 0.0);
    const std::vector<uint8_t> current = Render (scene, 1.0, 0.0, 9.0, 4.0);
    const pf::FrameMotion motion = pf::MeasureFrameMotion (View (previous), View (current));
    if (motion.valid) {
        // Horizontal strokes of finite length do carry end points; if the patches
        // found enough of them the answer has to be the right one.
        double dx = 0.0, dy = 0.0;
        pf::MotionAt (motion, 256.0, 192.0, dx, dy);
        EXPECT_NEAR (dx, 9.0, 0.25);
        EXPECT_NEAR (dy, 4.0, 0.25);
    }
}

// A frame that moved further than the search reaches is reported as unmatched.
TEST (PlanFrameRegistration, AMotionBeyondTheSearchIsNotMatchedToTheWrongPlace)
{
    const std::vector<Segment> scene = Scene ();
    const std::vector<uint8_t> previous = Render (scene, 1.0, 0.0, 0.0, 0.0);
    const std::vector<uint8_t> current = Render (scene, 1.0, 0.0, 150.0, -120.0);
    const pf::FrameMotion motion = pf::MeasureFrameMotion (View (previous), View (current));
    if (motion.valid) {
        double dx = 0.0, dy = 0.0;
        pf::MotionAt (motion, 256.0, 192.0, dx, dy);
        EXPECT_NEAR (dx, 150.0, 0.5);
        EXPECT_NEAR (dy, -120.0, 0.5);
    }
}

TEST (PlanFrameRegistration, MismatchedFramesAreRefused)
{
    const std::vector<uint8_t> frame (size_t (kWidth) * kHeight, 128);
    pf::GreyImage small = View (frame);
    small.width = 100;
    EXPECT_FALSE (pf::MeasureFrameMotion (View (frame), small).valid);
}
