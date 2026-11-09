#include "ArchViz/SunPathBand.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;
using namespace geomsrv::sunstudyhud;

namespace {
evp::sunstudy::SunStep At (int minute, double z = 0.8)
{
    evp::sunstudy::SunStep step;
    step.time = { minute / 60, minute % 60 };
    step.direction[2] = z;
    return step;
}
} // namespace

TEST (SunPathBand, AnalysisWindowIsHalfOpenAndHorizonGapsNeverBridge)
{
    SunPathStudyDay day;
    day.available = true;
    day.year = 2028;
    day.month = 2;
    day.day = 29;
    day.hourFrom = 10;
    day.hourTo = 12;
    day.timestepMinutes = 15;
    day.steps = { At (600), At (615), At (630, -0.1), At (645), At (675), At (720) };
    const auto band = BuildSunPathBand (day);
    ASSERT_EQ (band.days.size (), 1u);
    ASSERT_EQ (band.days[0].runs.size (), 3u);
    EXPECT_EQ (band.steps, 4u);
    EXPECT_EQ (band.days[0].runs[0].size (), 2u);
    EXPECT_EQ (band.days[0].runs[1].front ().minute, 645);
    EXPECT_EQ (band.days[0].runs[2].front ().minute, 675);
    EXPECT_FALSE (band.days[0].discrete);
}

TEST (SunPathBand, PeriodKeepsEveryTracedDayWeightAndSunlessDaySeparately)
{
    SunPathPeriodGeometry period;
    period.year = 2028;
    period.estimate = true;
    period.days = { { 1, 15, 31.0, 1.0, { At (600), At (660) } },
                    { 2, 15, 28.0, 1.0, {} },
                    { 2, 29, 1.0, 1.0, { At (600) } } };
    const auto band = BuildSunPathBand (period);
    ASSERT_EQ (band.days.size (), 3u);
    EXPECT_DOUBLE_EQ (band.days[0].weightDays, 31.0);
    EXPECT_TRUE (band.days[0].discrete);
    EXPECT_TRUE (band.days[1].runs.empty ());
    EXPECT_EQ (band.days[2].runs[0].front ().day, 29);
    EXPECT_EQ (band.steps, 3u);
}

TEST (SunPathBand, UnsortedDuplicateStepsHaveStableOrderWithoutMutatingTheSource)
{
    const std::vector<evp::sunstudy::SunStep> steps { At (630), At (600), At (615), At (615) };
    const auto day = BuildSunPathBandDay (2026, 6, 21, 1.0, 15, steps);
    ASSERT_EQ (day.runs.size (), 1u);
    ASSERT_EQ (day.runs[0].size (), 3u);
    EXPECT_EQ (day.runs[0][0].step, 1);
    EXPECT_EQ (day.runs[0][1].step, 2);
    EXPECT_EQ (day.runs[0][2].step, 0);
    EXPECT_EQ (steps[0].time.minute, 30);
}

TEST (SunPathBand, LodKeepsBothBoundariesAndTheSelectedExactStep)
{
    const auto indices = SunPathDisplayIndices (1000, 501, 100);
    ASSERT_FALSE (indices.empty ());
    EXPECT_EQ (indices.front (), 0u);
    EXPECT_EQ (indices.back (), 999u);
    EXPECT_TRUE (std::binary_search (indices.begin (), indices.end (), size_t (501)));
    EXPECT_LT (indices.size (), 1000u);
    EXPECT_EQ (SunPathDisplayIndices (3, 1), (std::vector<size_t> { 0, 1, 2 }));
    EXPECT_TRUE (SunPathDisplayIndices (0, 0).empty ());
}

TEST (SunPathBand, NormalizedCacheIgnoresAppliedViewAndRetiresWithItsOwner)
{
    SunPathStudyDay day;
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->year = 2028;
    geometry->days = { { 2, 29, 1.0, 0.25, { At (600), At (615) } } };
    auto period = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, {} });
    SunPathBandCache cache;
    cache.Update (day, period, true);
    EXPECT_EQ (cache.Data ().steps, 2u);
    const auto builds = cache.Builds ();
    period = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, { 1, 0, 0, 1 } });
    cache.Update (day, period, true);
    EXPECT_EQ (cache.Builds (), builds);
    cache.Update (day, nullptr, true);
    EXPECT_EQ (cache.Data ().steps, 0u);
    EXPECT_GT (cache.Builds (), builds);
}

TEST (SunPathBand, LodAlsoPreservesBothInteractiveSliceTransitions)
{
    std::vector<SunPathMoment> run;
    for (int minute = 0; minute < 1000; ++minute) {
        SunPathMoment moment;
        moment.minute = minute;
        run.push_back (moment);
    }
    const auto indices = SunPathRunDisplayIndices (run, 501, 603, 679);
    for (const size_t index : { 0u, 501u, 602u, 603u, 678u, 679u, 999u })
        EXPECT_TRUE (std::binary_search (indices.begin (), indices.end (), index));
}
