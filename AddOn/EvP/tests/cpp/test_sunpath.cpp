#include "ArchViz/SunPathCompass.hpp"

#include <gtest/gtest.h>
#include <limits>

using namespace geomsrv::archviz;

TEST (SunPath, DomeUsesSceneCentreAndFloorAtEveryScale)
{
    const double lo[3] = { 100.0, -40.0, 8.0 }, hi[3] = { 130.0, 0.0, 28.0 };
    double centre[3] = {}, radius = 0.0;
    ASSERT_TRUE (SunPathSceneDome (lo, hi, 1.0f, centre, radius));
    EXPECT_DOUBLE_EQ (centre[0], 115.0);
    EXPECT_DOUBLE_EQ (centre[1], -20.0);
    EXPECT_DOUBLE_EQ (centre[2], 8.0);
    EXPECT_DOUBLE_EQ (radius, 30.0);
    ASSERT_TRUE (SunPathSceneDome (lo, hi, 4.0f, centre, radius));
    EXPECT_DOUBLE_EQ (radius, 120.0);
    EXPECT_DOUBLE_EQ (centre[2], 8.0);
    const double invalid[3] = { std::numeric_limits<double>::quiet_NaN (), 0.0, 0.0 };
    EXPECT_FALSE (SunPathSceneDome (invalid, hi, 1.0f, centre, radius));
    EXPECT_FALSE (SunPathSceneDome (hi, lo, 1.0f, centre, radius));
}

TEST (SunPath, CompassBaselineFollowsProjectedTangentWithoutUprightFlips)
{
    // An oblique projection maps a ground circle to an ellipse. Its tangent
    // is not perpendicular to the screen radial except at the axes.
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 360; ++i) {
        const double t = double (i) * pi / 180.0;
        const double tx = -std::sin (t), ty = 0.25 * std::cos (t);
        const double angle = SunPathCompassAngle (tx, ty);
        const double length = std::hypot (tx, ty);
        EXPECT_NEAR (std::cos (angle), tx / length, 1.0e-12);
        EXPECT_NEAR (std::sin (angle), ty / length, 1.0e-12);
    }
}

TEST (SunPath, HoverCaptionNamesActualSunTimeAndDisplayedAnalysis)
{
    EXPECT_EQ (SunPathMomentCaption (2028, 2, 29, 725), "Sun: 2028-02-29  12:05");
    EXPECT_EQ (SunPathMomentCaption (2026, 12, 21, 0), "Sun: 2026-12-21  00:00");
    EXPECT_TRUE (SunPathMomentCaption (2026, 0, 1, 720).empty ());
    EXPECT_TRUE (SunPathMomentCaption (2026, 1, 1, -1).empty ());
    EXPECT_EQ (SunPathAnalysisCaption (true, "2026 year", 20260621, 2027, 1, 1), "Analysis period: 2026 year");
}
