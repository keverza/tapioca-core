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

TEST (SunPath, HoverCaptionCarriesOnlyTheSunAndAnalysisNotBandDiagnostics)
{
    const auto moment = SunPathMomentCaption (2028, 2, 29, 725);
    const auto analysis = SunPathAnalysisCaption (true, "2026-03-21 to 2026-09-23", 20280229, 2027, 1, 1);
    const auto caption = SunPathHoverCaption (moment, analysis);
    EXPECT_EQ (caption, "Sun: 2028-02-29  12:05\nAnalysis period: 2026-03-21 to 2026-09-23");
    EXPECT_EQ (caption.find ("analysis steps"), std::string::npos);
    EXPECT_EQ (caption.find ("before clipping"), std::string::npos);
    EXPECT_EQ (SunPathHoverCaption ("", analysis), analysis);
    EXPECT_EQ (SunPathHoverCaption (moment, ""), moment);
    EXPECT_TRUE (SunPathHoverCaption ("", "").empty ());
}

TEST (SunPath, CaptureLegendLocationNamesHemispheresAltitudeAndNorth)
{
    const auto lines = SunStudyCaptureLocationLines (true, 54.6872f, -25.2797f, 112.0f, 90.0f);
    ASSERT_EQ (lines.size (), 3u);
    EXPECT_EQ (lines[0], "54.6872 N, 25.2797 W");
    EXPECT_EQ (lines[1], "Altitude 112 m");
    EXPECT_EQ (lines[2], "North 90.0 deg CCW from +X");
    EXPECT_EQ (SunStudyCaptureLocationLines (true, -33.8688f, 151.2093f, 0.0f, 0.0f)[0], "33.8688 S, 151.2093 E");
    // No place ever arrived: say so, never "0.0000 N, 0.0000 E".
    const auto unknown = SunStudyCaptureLocationLines (false, 0.0f, 0.0f, 0.0f, 90.0f);
    ASSERT_EQ (unknown.size (), 1u);
    EXPECT_EQ (unknown[0], "Location not received from Archicad");
}

TEST (SunPath, CaptureLegendJoinsEachHalfLineWithBars)
{
    EXPECT_EQ (SunStudyCaptureJoin (SunStudyCaptureLocationLines (true, 54.6736f, 25.2250f, 120.0f, 90.0f)),
               "54.6736 N, 25.2250 E | Altitude 120 m | North 90.0 deg CCW from +X");
    EXPECT_EQ (SunStudyCaptureJoin ({ "2026-03-21, one day" }), "2026-03-21, one day");
    EXPECT_TRUE (SunStudyCaptureJoin ({}).empty ());
}

TEST (SunPath, CaptureLegendPeriodSaysWhenAndWhichDayTheShadowsAre)
{
    SunStudyCapturePeriod day;
    day.shownDate = 20260621;
    day.firstMinute = 5 * 60 + 15;
    day.lastMinute = 21 * 60 + 45;
    day.stepMinutes = 15;
    auto lines = SunStudyCapturePeriodLines (day);
    ASSERT_EQ (lines.size (), 2u);
    EXPECT_EQ (lines[0], "2026-06-21, one day");
    EXPECT_EQ (lines[1], "Traced 05:15-21:45, every 15 min");
    // A single day's shadows ARE the study's day: no extra line.
    day.shadowsOfOneDay = true;
    EXPECT_EQ (SunStudyCapturePeriodLines (day).size (), 2u);

    SunStudyCapturePeriod period;
    period.showsPeriod = true;
    period.periodLabel = "2026-03-21 to 2026-09-23";
    period.estimate = true;
    period.carrierDate = "2026-09-23";
    lines = SunStudyCapturePeriodLines (period);
    ASSERT_EQ (lines.size (), 1u);
    EXPECT_EQ (lines[0], "2026-03-21 to 2026-09-23 (estimate)");
    // A period frame showing shadows names the one traced day they are of.
    period.shadowsOfOneDay = true;
    lines = SunStudyCapturePeriodLines (period);
    ASSERT_EQ (lines.size (), 2u);
    EXPECT_EQ (lines[1], "Shadows of 2026-09-23 only");

    // Unknown date is said, not invented from a pending run form.
    EXPECT_EQ (SunStudyCapturePeriodLines (SunStudyCapturePeriod {})[0], "Date unknown");
}
