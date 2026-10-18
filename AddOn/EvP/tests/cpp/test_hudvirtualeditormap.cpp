#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace hud = geomsrv::archviz::overlayhud;

namespace {
ed::WorkingPlane Tilted ()
{
    auto plane = ed::WorldXY (12);
    plane.source = ed::PlaneSource::Explicit;
    const double c = std::sqrt (0.5);
    plane.v = { 0, c, c }, plane.normal = { 0, -c, c };
    return plane;
}

hud::Input Plan ()
{
    hud::Input input;
    input.planar = true;
    input.plan[0] = 20, input.plan[1] = 3, input.plan[2] = 600;
    input.plan[3] = 2, input.plan[4] = -20, input.plan[5] = 500;
    return input;
}
} // namespace

TEST (HudVirtualEditorMap, PlanRoundTripsAnObliqueFrameAndItsLocalScale)
{
    const auto input = Plan ();
    const eh::detail::Map map { input };
    auto plane = Tilted ();
    plane.origin.x = 7, plane.origin.y = -9;
    const auto point = ed::ToWorld (plane, { 3, 4, 0 });
    ImVec2 pixel;
    ASSERT_TRUE (map.Screen (point, pixel));
    ed::Vec3 out;
    ASSERT_TRUE (map.Onto (pixel.x, pixel.y, plane, {}, out));
    EXPECT_NEAR (ed::Distance (point, out), 0, 1e-5);
    EXPECT_NEAR (ed::ToLocal (plane, out).z, 0, 1e-12);
    double scale = 0;
    ASSERT_TRUE (map.MetresPerPixel (pixel.x, pixel.y, plane, out, scale));
    ed::Vec3 adjacent;
    ASSERT_TRUE (map.Onto (pixel.x + 1, pixel.y, plane, out, adjacent));
    EXPECT_NEAR (scale, ed::Distance (out, adjacent), 1e-12);
}

TEST (HudVirtualEditorMap, PerspectiveRoundTripsInTheAuthoredPlaneWithoutCameraAcquisition)
{
    hud::Input input;
    input.planar = false;
    input.project = [] (double x, double y, double z, float& px, float& py) {
        const double w = 1 + 0.005 * z;
        if (w <= 0)
            return false;
        px = float (600 + (20 * x + 2 * z) / w), py = float (500 - (20 * y + 5 * z) / w);
        return true;
    };
    const eh::detail::Map map { input };
    const auto plane = Tilted ();
    for (const auto local : { ed::Vec3 { 3, 4, 0 }, ed::Vec3 { -5, 30, 0 } }) {
        const auto world = ed::ToWorld (plane, local);
        ImVec2 pixel, back;
        ASSERT_TRUE (map.Screen (world, pixel));
        ed::Vec3 out;
        ASSERT_TRUE (map.Onto (pixel.x, pixel.y, plane, plane.origin + plane.normal * 100, out));
        EXPECT_NEAR (ed::ToLocal (plane, out).z, 0, 1e-12);
        ASSERT_TRUE (map.Screen (out, back));
        EXPECT_LT (std::hypot (back.x - pixel.x, back.y - pixel.y), 0.05);
    }
}

TEST (HudVirtualEditorMap, VerticalPlaneMapsIn3DButRefusesInPlanEvenAtTheSeed)
{
    auto input = Plan ();
    auto plane = ed::WorldXY (12);
    plane.source = ed::PlaneSource::Explicit;
    plane.u = { 0, 1, 0 }, plane.v = { 0, 0, 1 }, plane.normal = { 1, 0, 0 };
    ed::Vec3 out { 7, 8, 9 };
    EXPECT_FALSE ((eh::detail::Map { input }.Onto (600, 500, plane, plane.origin, out)));
    EXPECT_EQ (out, (ed::Vec3 { 7, 8, 9 }));
    input.planar = false;
    input.project = [] (double, double y, double z, float& px, float& py) {
        px = float (600 + 20 * y), py = float (500 - 20 * z);
        return true;
    };
    ASSERT_TRUE ((eh::detail::Map { input }.Onto (660, 100, plane, {}, out)));
    EXPECT_NEAR (ed::Distance (out, { 0, 3, 20 }), 0, 1e-9);
}

TEST (HudVirtualEditorMap, EdgeOnMatchingPixelDoesNotBecomeAnArbitraryHitOrScale)
{
    hud::Input input;
    input.planar = false;
    input.project = [] (double x, double, double z, float& px, float& py) {
        px = float (600 + 20 * x), py = float (500 - 20 * z);
        return true;
    };
    const eh::detail::Map map { input };
    ed::Vec3 out { 7, 8, 9 };
    double scale = 123;
    EXPECT_FALSE (map.Onto (600, 400, ed::WorldXY (5), { 0, 100, 5 }, out));
    EXPECT_EQ (out, (ed::Vec3 { 7, 8, 9 }));
    EXPECT_FALSE (map.MetresPerPixel (600, 400, ed::WorldXY (5), {}, scale));
    EXPECT_DOUBLE_EQ (scale, 123);
}

TEST (HudVirtualEditorMap, InvalidFramesAndProjectionFailuresLeaveNoCandidate)
{
    auto input = Plan ();
    auto plane = Tilted ();
    plane.v = plane.u;
    ed::Vec3 out { 7, 8, 9 };
    EXPECT_FALSE ((eh::detail::Map { input }.Onto (600, 400, plane, {}, out)));
    plane = Tilted ();
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE ((eh::detail::Map { input }.Onto (600, 400, plane, { nan, 0, 0 }, out)));
    input.plan[2] = nan;
    EXPECT_FALSE ((eh::detail::Map { input }.Onto (600, 400, plane, {}, out)));
    input.planar = false;
    EXPECT_FALSE ((eh::detail::Map { input }.Onto (600, 400, plane, {}, out))); // no supplied projection
    input.project = [] (double, double, double, float&, float&) { return false; };
    EXPECT_FALSE ((eh::detail::Map { input }.Onto (600, 400, plane, {}, out)));
    EXPECT_EQ (out, (ed::Vec3 { 7, 8, 9 }));
}

TEST (HudVirtualEditorMap, PlanInversionKeepsFarFromOriginDatumsInDoublePrecision)
{
    auto input = Plan ();
    auto plane = ed::WorldXY (103.25);
    plane.origin.x = 1e8, plane.origin.y = -2e8;
    input.plan[1] = input.plan[3] = 0;
    input.plan[2] = 600 - 20 * plane.origin.x;
    input.plan[5] = 500 + 20 * plane.origin.y;
    ed::Vec3 out;
    ASSERT_TRUE ((eh::detail::Map { input }.Onto (660, 420, plane, {}, out)));
    EXPECT_EQ (out, (ed::Vec3 { 1e8 + 3, -2e8 + 4, 103.25 }));
}
