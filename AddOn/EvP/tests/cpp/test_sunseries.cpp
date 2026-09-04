// L2 offline tests for SunStudy/SunSeries.
//
// The oracle is private/Commands/SunStudy/sunseries.py. What matters here is not
// arithmetic (a division by 60 needs no defending) but the three behaviours a
// careless change would take away: the whole-day default, the zero-degree
// horizon floor, and a version that cannot be forgotten.

#include "SunStudy/SunSeries.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace evp::sunstudy;

namespace {

SunStep MakeStep (int hour, int minute, double altitude, double x = 0.0, double y = 0.0, double z = 1.0)
{
    SunStep step;
    step.time = TimeOfDay { hour, minute };
    step.altitudeDegrees = altitude;
    step.direction[0] = x;
    step.direction[1] = y;
    step.direction[2] = z;
    return step;
}

} // namespace

// ---------------------------------------------------------------------------
// timestep enumeration
// ---------------------------------------------------------------------------

TEST (SunSeries, WholeDayIsTheDefaultWindow)
{
    EXPECT_EQ (EnumerateTimesteps (60).size (), 24u);
    EXPECT_EQ (EnumerateTimesteps (30).size (), 48u);
    EXPECT_EQ (EnumerateTimesteps (15).size (), 96u);

    const std::vector<TimeOfDay> hourly = EnumerateTimesteps (60);
    EXPECT_EQ (hourly.front ().hour, 0);
    EXPECT_EQ (hourly.front ().minute, 0);
    EXPECT_EQ (hourly.back ().hour, 23);
}

TEST (SunSeries, TheWindowUpperBoundIsExclusive)
{
    const std::vector<TimeOfDay> window = EnumerateTimesteps (60, 8, 20);
    ASSERT_EQ (window.size (), 12u);
    EXPECT_EQ (window.front ().hour, 8);
    EXPECT_EQ (window.back ().hour, 19) << "20:00 must not be included";
}

TEST (SunSeries, MinutesLandOnTheStep)
{
    const std::vector<TimeOfDay> quarter = EnumerateTimesteps (15, 9, 10);
    ASSERT_EQ (quarter.size (), 4u);
    EXPECT_EQ (quarter[0].minute, 0);
    EXPECT_EQ (quarter[1].minute, 15);
    EXPECT_EQ (quarter[2].minute, 30);
    EXPECT_EQ (quarter[3].minute, 45);
}

TEST (SunSeries, DegenerateWindowsAndStepsAreClampedNotCrashed)
{
    EXPECT_TRUE (EnumerateTimesteps (60, 20, 8).empty ()) << "an inverted window is empty";
    EXPECT_EQ (EnumerateTimesteps (60, -5, 30).size (), 24u) << "clamped to the day";
    EXPECT_EQ (EnumerateTimesteps (0).size (), 1440u) << "a zero step becomes one minute";
    EXPECT_EQ (EnumerateTimesteps (-10).size (), 1440u);
}

// ---------------------------------------------------------------------------
// the horizon filter
// ---------------------------------------------------------------------------

TEST (SunSeries, StepsAtOrBelowTheFloorAreDropped)
{
    const std::vector<SunStep> steps = {
        MakeStep (5, 0, -10.0), MakeStep (6, 0, 0.0),   MakeStep (7, 0, 0.5),
        MakeStep (12, 0, 45.0), MakeStep (20, 0, -1.0),
    };
    const SunSeries series = SunSeries::FromSteps (steps, 60);

    ASSERT_EQ (series.StepCount (), 2u) << "0.0 is not above a 0.0 floor";
    EXPECT_EQ (series.Step (0).time.hour, 7);
    EXPECT_EQ (series.Step (1).time.hour, 12);
    EXPECT_EQ (series.SourceStepCount (), 5u) << "the report needs to know what was dropped";
}

// ⚠️ THE FLOOR DEFAULTS TO ZERO, AND RAISING IT BIASES EVERY TOTAL DOWN.
TEST (SunSeries, ARaisedFloorDiscardsTheEndsOfTheDay)
{
    const std::vector<SunStep> steps = {
        MakeStep (6, 0, 1.0),
        MakeStep (7, 0, 2.5),
        MakeStep (12, 0, 45.0),
        MakeStep (18, 0, 2.0),
    };

    EXPECT_EQ (SunSeries::FromSteps (steps, 60, 0.0).StepCount (), 4u);
    EXPECT_EQ (SunSeries::FromSteps (steps, 60, 3.0).StepCount (), 1u) << "a 3-degree floor drops three of four";
}

// ⚠️ A ZERO-LENGTH DIRECTION IS DROPPED EVEN WITH A PLAUSIBLE ALTITUDE. It is
// what the host reports for a sun it could not resolve, and normalising it gives
// NaN that then silences a whole timestep instead of failing.
TEST (SunSeries, AZeroDirectionIsDroppedRatherThanNormalisedToNaN)
{
    const std::vector<SunStep> steps = {
        MakeStep (12, 0, 45.0, 0.0, 0.0, 0.0),
        MakeStep (13, 0, 40.0, 0.0, 0.0, 1.0),
    };
    const SunSeries series = SunSeries::FromSteps (steps, 60);

    ASSERT_EQ (series.StepCount (), 1u);
    EXPECT_EQ (series.Step (0).time.hour, 13);
    EXPECT_FALSE (std::isnan (series.Step (0).direction[2]));
}

TEST (SunSeries, DirectionsComeBackNormalised)
{
    const std::vector<SunStep> steps = { MakeStep (12, 0, 45.0, 3.0, 0.0, 4.0) };
    const SunSeries series = SunSeries::FromSteps (steps, 60);

    ASSERT_EQ (series.StepCount (), 1u);
    const double* d = series.Step (0).direction;
    EXPECT_NEAR (std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1.0, 1e-12);
    EXPECT_NEAR (d[0], 0.6, 1e-12);
    EXPECT_NEAR (d[2], 0.8, 1e-12);
}

// ---------------------------------------------------------------------------
// hours arithmetic
// ---------------------------------------------------------------------------

TEST (SunSeries, DaylightHoursIsTheDenominatorOfEveryPercentage)
{
    std::vector<SunStep> steps;
    for (int i = 0; i < 46; ++i)
        steps.push_back (MakeStep (i / 4, (i % 4) * 15, 10.0));

    const SunSeries series = SunSeries::FromSteps (steps, 15);
    EXPECT_EQ (series.StepCount (), 46u);
    EXPECT_NEAR (series.HoursPerStep (), 0.25, 1e-12);
    EXPECT_NEAR (series.DaylightHours (), 11.5, 1e-12);
}

// ---------------------------------------------------------------------------
// the version
// ---------------------------------------------------------------------------

TEST (SunSeries, VersionChangesWithEverythingThatChangesTheAnswer)
{
    const std::vector<SunStep> base = { MakeStep (9, 0, 20.0, 1.0, 0.0, 1.0), MakeStep (12, 0, 45.0, 0.0, 0.0, 1.0) };
    const uint64_t reference = SunSeries::FromSteps (base, 60).Version ();
    EXPECT_NE (reference, 0u);

    // Same input, same version: a cache hit has to be possible.
    EXPECT_EQ (SunSeries::FromSteps (base, 60).Version (), reference);

    // A different timestep is a different day's worth of steps.
    EXPECT_NE (SunSeries::FromSteps (base, 30).Version (), reference);

    // A moved sun.
    std::vector<SunStep> moved = base;
    moved[1].direction[0] = 0.3;
    EXPECT_NE (SunSeries::FromSteps (moved, 60).Version (), reference);

    // A different time of day at the same position.
    std::vector<SunStep> shifted = base;
    shifted[0].time.minute = 30;
    EXPECT_NE (SunSeries::FromSteps (shifted, 60).Version (), reference);

    // A raised floor that actually drops a step.
    EXPECT_NE (SunSeries::FromSteps (base, 60, 30.0).Version (), reference);
}

TEST (SunSeries, EmptyIsVersionZeroSoItIsNotMistakenForUnchanged)
{
    const std::vector<SunStep> nightOnly = { MakeStep (2, 0, -30.0) };
    const SunSeries series = SunSeries::FromSteps (nightOnly, 60);

    EXPECT_TRUE (series.Empty ());
    EXPECT_EQ (series.Version (), 0u);
    EXPECT_NEAR (series.DaylightHours (), 0.0, 1e-12);
    EXPECT_EQ (series.SourceStepCount (), 1u);
}

// -0.0 and 0.0 are equal but differently shaped in memory. Without
// normalisation a sun vector differing only in a zero's sign would read as a
// different day and throw away a valid cache.
TEST (SunSeries, NegativeZeroDoesNotChangeTheVersion)
{
    const std::vector<SunStep> positive = { MakeStep (12, 0, 45.0, 0.0, 0.0, 1.0) };
    const std::vector<SunStep> negative = { MakeStep (12, 0, 45.0, -0.0, -0.0, 1.0) };
    EXPECT_EQ (SunSeries::FromSteps (positive, 60).Version (), SunSeries::FromSteps (negative, 60).Version ());
}
