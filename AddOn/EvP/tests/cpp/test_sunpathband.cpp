#include "ArchViz/SunPathBand.hpp"
#include "ArchViz/SunPathBandDraw.hpp"

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

TEST (SunPathBand, SelectedLookupUsesSortedMinuteAndExactStepWithDuplicatesAndFiltering)
{
    std::vector<SunPathMoment> run (5);
    for (size_t i = 0; i < run.size (); ++i)
        run[i].valid = true;
    run[0].minute = 99;
    run[0].step = 4;
    run[1].minute = 100;
    run[1].step = 6;
    run[2].minute = 100;
    run[2].step = 7;
    run[3].minute = 100;
    run[3].step = 9;
    run[4].minute = 200;
    run[4].step = 7;
    SunPathMoment selected;
    selected.valid = true;
    selected.minute = 100;
    selected.step = 7;
    EXPECT_EQ (SunPathSelectedRunIndex (run, selected), 2u);
    selected.step = 8; // same clock, but no such source step
    EXPECT_EQ (SunPathSelectedRunIndex (run, selected), run.size ());
    selected.minute = 101;
    selected.step = 7; // same source step occurs later, outside this clock
    EXPECT_EQ (SunPathSelectedRunIndex (run, selected), run.size ());

    const auto filtered = BuildSunPathBandDay (2028, 2, 29, 1.0, 15, { At (50), At (100) }, 60, 120);
    ASSERT_EQ (filtered.runs.size (), 1u);
    selected = SunPathStepMoment (2028, 2, 29, { At (50), At (100) }, 0);
    EXPECT_EQ (SunPathSelectedRunIndex (filtered.runs[0], selected), filtered.runs[0].size ());
}

TEST (SunPathBand, SmallGlobalRunLimitStillPreservesCurrentAndSliceTransitions)
{
    std::vector<SunPathMoment> run (1000);
    for (size_t i = 0; i < run.size (); ++i) {
        run[i].valid = true;
        run[i].minute = int (i);
        run[i].step = int (i);
    }
    SunPathMoment selected = run[1];
    const size_t selectedIndex = SunPathSelectedRunIndex (run, selected);
    ASSERT_EQ (selectedIndex, 1u);
    std::vector<size_t> indices;
    AppendSunPathRunDisplayIndices (run, selectedIndex, 603, 679, SunPathBandRunLimit (2400), indices);
    for (const size_t index : { 0u, 1u, 602u, 603u, 678u, 679u, 999u })
        EXPECT_TRUE (std::binary_search (indices.begin (), indices.end (), index));
    EXPECT_LE (indices.size (), 8u); // two-stride LOD sample plus all required preservation points
}

TEST (SunPathBand, InactiveAlphaStaysWarmAndLegibleForWholeYearStudies)
{
    EXPECT_EQ (SunPathBandInactiveAlpha (1), 55);
    EXPECT_EQ (SunPathBandInactiveAlpha (365), kSunPathBandInactiveAlphaFloor);
    EXPECT_EQ (SunPathBandInactiveAlpha (100000), kSunPathBandInactiveAlphaFloor);
    for (const size_t days : { 1u, 2u, 10u, 55u, 100u, 365u, 1000u }) {
        const int inactive = SunPathBandInactiveAlpha (days);
        EXPECT_GE (inactive, kSunPathBandInactiveAlphaFloor);
        EXPECT_LE (inactive, 55);
        EXPECT_GT (SunPathBandActiveAlphaFor (days), inactive + 100)
            << "the current/highlighted ribbon must read clearly above the field";
    }
    EXPECT_EQ (SunPathBandColour (kSunPathBandInactiveAlphaFloor),
               IM_COL32 (255, 225, 90, kSunPathBandInactiveAlphaFloor));
}

TEST (SunPathBand, GlobalBudgetBoundsManyRunStudiesAndKeepsRunEndpoints)
{
    EXPECT_EQ (SunPathBandRunLimit (1), kSunPathBandMaxRunSamples);
    EXPECT_EQ (SunPathBandRunLimit (365), 6u);
    EXPECT_EQ (SunPathBandRunLimit (1000), kSunPathBandMinRunSamples);

    // 300 runs of 200 samples: one global budget, not 300 separate LODs.
    std::vector<SunPathMoment> run;
    for (int minute = 0; minute < 200; ++minute) {
        SunPathMoment moment;
        moment.minute = minute;
        moment.step = minute;
        run.push_back (moment);
    }
    const size_t limit = SunPathBandRunLimit (300);
    size_t shown = 0;
    std::vector<size_t> indices;
    for (int day = 0; day < 300; ++day) {
        AppendSunPathRunDisplayIndices (run, run.size (), 0, 1440, limit, indices);
        ASSERT_GE (indices.size (), 2u);
        EXPECT_EQ (indices.front (), 0u);
        EXPECT_EQ (indices.back (), run.size () - 1);
        EXPECT_LT (indices.size (), run.size ());
        shown += indices.size ();
    }
    EXPECT_LE (shown, kSunPathBandSampleBudget + 2 * 300);
}
