#include "NativeCommands/SunStudySeasonal.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::sunstudyhud;

TEST (SunStudySeasonal, ClocksWeightOnlyTracedDaysAndMeasuredSteps)
{
    PeriodProfile profile;
    profile.samples = { 0, 1 };
    profile.dayMonth = { 3, 4, 6, 12 };
    profile.dayWeights = { 2.0, 1.0, 4.0, 3.0 };
    profile.dayHoursPerStep = { 0.25, 0.25, 0.25, 0.25 };
    profile.dayLit = { 0.25, 0.25, 0.0, 0.25 };
    profile.dayFacing = { 0.25, 0.5, 0.0, 0.25 };
    profile.dayUp = { 0.25, 0.5, 0.0, 0.25 };
    profile.dayMinutes = { { 480 }, { 480, 540 }, {}, { 600 } };
    profile.dayStepLit = { { 1.0 }, { 0.0, 1.0 }, {}, { 1.0 } };
    profile.dayStepFacing = { { 1.0 }, { 1.0, 1.0 }, {}, { 1.0 } };

    const auto seasons = SeasonalPointDays (profile);
    EXPECT_EQ (seasons[0].label, "DJF");
    EXPECT_EQ (seasons[0].tracedDays, 1u);
    EXPECT_DOUBLE_EQ (seasons[0].representedDays, 3.0);
    EXPECT_EQ (seasons[0].day.minutes, (std::vector<uint16_t> { 600 }));
    EXPECT_EQ (seasons[1].label, "MAM");
    EXPECT_EQ (seasons[1].tracedDays, 2u);
    EXPECT_DOUBLE_EQ (seasons[1].representedDays, 3.0);
    ASSERT_EQ (seasons[1].day.minutes, (std::vector<uint16_t> { 480, 540 }));
    EXPECT_NEAR (seasons[1].day.lit[0], 2.0 / 3.0, 1.0e-12);
    EXPECT_DOUBLE_EQ (seasons[1].day.lit[1], 1.0); // only the day that measured 09:00 is its denominator
    EXPECT_DOUBLE_EQ (seasons[1].day.facing[0], 1.0);
    EXPECT_EQ (seasons[1].day.samples, 2u);
    EXPECT_TRUE (seasons[1].day.hasTotalHours);
    EXPECT_DOUBLE_EQ (seasons[1].day.directHours, 0.25); // weighted daily mean, not the integral of conditional rings
    EXPECT_DOUBLE_EQ (seasons[1].day.facingHours, 1.0 / 3.0);
    EXPECT_DOUBLE_EQ (seasons[1].day.sunUpHours, 1.0 / 3.0);
    EXPECT_FALSE (seasons[1].day.hasCellRange);
    EXPECT_EQ (seasons[2].label, "JJA");
    EXPECT_EQ (seasons[2].tracedDays, 1u); // the empty measured day remains represented, with no invented frame
    EXPECT_DOUBLE_EQ (seasons[2].representedDays, 4.0);
    EXPECT_DOUBLE_EQ (seasons[2].day.directHours, 0.0);
    EXPECT_TRUE (seasons[2].day.minutes.empty ());
    EXPECT_EQ (seasons[3].tracedDays, 0u);
    EXPECT_TRUE (seasons[3].day.minutes.empty ());
}
