#include "ArchViz/SunPathMoment.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;
using namespace geomsrv::sunstudyhud;

namespace {
evp::sunstudy::SunStep Step (int minute, double z = 0.75)
{
    evp::sunstudy::SunStep step;
    step.time = { minute / 60, minute % 60 };
    step.direction[0] = 0.6;
    step.direction[1] = 0.2;
    step.direction[2] = z;
    return step;
}
} // namespace

TEST (SunPathMoment, ExactLeapDayNeverUsesTheNonLeapLattice)
{
    const std::vector<evp::sunstudy::SunStep> steps { Step (725) };
    const auto sun = SunPathStepMoment (2028, 2, 29, steps, 0);
    ASSERT_TRUE (sun.valid);
    EXPECT_EQ (sun.year, 2028);
    EXPECT_EQ (sun.month, 2);
    EXPECT_EQ (sun.day, 29);
    EXPECT_EQ (sun.minute, 725);
    EXPECT_FLOAT_EQ (sun.direction[0], 0.6f);
    EXPECT_FLOAT_EQ (sun.direction[2], 0.75f);
    EXPECT_FALSE (SunPathStepMoment (2028, 2, 29, steps, 1).valid);
    EXPECT_FALSE (SunPathStepMoment (2028, 2, 29, {}, 0).valid);
}

TEST (SunPathMoment, NearestIsDeterministicInsideTheHalfOpenWindow)
{
    const std::vector<evp::sunstudy::SunStep> steps { Step (780), Step (720), Step (720), Step (690),
                                                      Step (750, -0.1) };
    auto sun = SunPathNearestMoment (2026, 6, 21, steps, 750);
    ASSERT_TRUE (sun.valid);
    EXPECT_EQ (sun.minute, 720); // equidistant clocks: earlier wins, not input order
    EXPECT_EQ (sun.step, 1);     // duplicate clock: first stored index wins
    sun = SunPathNearestMoment (2026, 6, 21, steps, 750, 721, 781);
    EXPECT_EQ (sun.minute, 780);
    EXPECT_FALSE (SunPathNearestMoment (2026, 6, 21, steps, 750, 721, 780).valid);
}

TEST (SunPathMoment, MonthHourHoverSearchesAllTracedDaysInsideTheHour)
{
    SunPathPeriodGeometry geometry;
    geometry.year = 2028;
    geometry.estimate = true;
    geometry.days = { { 2, 15, 14.0, 0.25, { Step (720) } },
                      { 2, 29, 15.0, 0.25, { Step (630), Step (645) } },
                      { 3, 15, 31.0, 0.25, { Step (630) } } };
    const auto sun = SunPathMonthMoment (geometry, 2, 630, 600, 660);
    ASSERT_TRUE (sun.valid);
    EXPECT_EQ (sun.day, 29);
    EXPECT_EQ (sun.minute, 630);
    EXPECT_FALSE (SunPathMonthMoment (geometry, 2, 590, 540, 600).valid);
    EXPECT_DOUBLE_EQ (geometry.days[1].weightDays, 15.0);
}

TEST (SunPathMoment, AppliedEnvelopeUsesExactShownStepAndAggregateHasNoInstant)
{
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->year = 2028;
    geometry->days = { { 2, 29, 1.0, 0.25, { Step (630), Step (645) } } };
    SunPathPeriodData data { geometry, { 1, 0, 0, 1 } };
    EXPECT_EQ (SunPathAppliedMoment (data).minute, 645);
    const auto original = data.geometry;
    data.applied = { 1, 0, 0, 0 };
    EXPECT_EQ (SunPathAppliedMoment (data).minute, 630);
    EXPECT_EQ (data.geometry, original);
    data.applied.frame = 2;
    EXPECT_FALSE (SunPathAppliedMoment (data).valid);
    data.applied.frame = 0;
    EXPECT_FALSE (SunPathAppliedMoment (data).valid);
}

TEST (SunPathMoment, CurrentHoverWinsAndInactivePlayersCannotStealShadowSelection)
{
    const std::vector<evp::sunstudy::SunStep> steps { Step (600), Step (720), Step (900) };
    SunPathSelection selection;
    selection.day = SunPathStepMoment (2026, 6, 21, steps, 0);
    selection.map = SunPathStepMoment (2026, 6, 21, steps, 1);
    selection.shadow = SunPathStepMoment (2026, 6, 21, steps, 2);
    selection.hover = selection.day;
    selection.mapActive = selection.shadowActive = true;
    selection.source = SunPathSyncSource::Shadow;
    EXPECT_EQ (ResolveSunPathMoment (selection).minute, 900);
    selection.hoverCurrent = true;
    EXPECT_EQ (ResolveSunPathMoment (selection).minute, 600);
    selection.hoverCurrent = false;
    selection.source = SunPathSyncSource::PointPlayer; // paused/hidden point player
    EXPECT_EQ (ResolveSunPathMoment (selection).minute, 900);
    selection.source = SunPathSyncSource::MapPlayer;
    selection.mapActive = false; // collapsed map player
    EXPECT_EQ (ResolveSunPathMoment (selection).minute, 900);
    selection.shadowActive = false;
    selection.aggregate = true;
    selection.map = {};
    EXPECT_FALSE (ResolveSunPathMoment (selection).valid); // never invent noon for totals
}
