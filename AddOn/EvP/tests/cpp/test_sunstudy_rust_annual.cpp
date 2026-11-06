// The annual accumulator through its C++ wrapper (#152): the schedule, a day
// study folded in at its weight, totals and means a day, presentation through
// the day's own core, and the refusals that keep a period on one scene.

#include "SunStudy/ITraversal.hpp"
#include "SunStudy/SunSeries.hpp"
#include "SunStudy/SunStudyRustAnnual.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include <memory>
#include <string>
#include <vector>

namespace {

using namespace evp::sunstudy;
namespace rs = evp::sunstudy::rust;

// Occluded east of x = 3, clear elsewhere.
class HalfShade final : public ITraversal {
  public:
    void OccludeDirectional (const double* origins, size_t count, const double*, double, double, uint8_t* out,
                             size_t) const override
    {
        for (size_t i = 0; i < count; ++i)
            out[i] = origins[3 * i] > 3.0 ? 1 : 0;
    }
    void OccludeRays (const OcclusionRay*, size_t count, uint8_t* out, size_t) const override
    {
        std::fill (out, out + count, uint8_t (0));
    }
    uint64_t SceneVersion () const override
    {
        return 1;
    }
};

SunSeries ThreeHours ()
{
    std::vector<SunStep> steps;
    for (int hour = 10; hour < 13; ++hour) {
        SunStep step;
        step.time = TimeOfDay { hour, 0 };
        step.direction[2] = 1.0;
        step.altitudeDegrees = 40.0;
        steps.push_back (step);
    }
    return SunSeries::FromSteps (steps, 60);
}

std::unique_ptr<rs::RustStudyCore> Ground (const std::vector<double>& positions)
{
    std::string error;
    auto core = rs::RustStudyCore::Ground (ThreeHours (), 1.0, positions, {}, error);
    EXPECT_NE (core, nullptr) << error;
    return core;
}

void Converge (const rs::RustStudyCore& core)
{
    const HalfShade traversal;
    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (core.Sync (StudyInputs {}, error)) << error;
    ASSERT_TRUE (core.Advance (traversal, 100, 0.001, 0.0, 1, {}, {}, advanced, error)) << error;
    StudyProgress progress;
    ASSERT_TRUE (core.Progress (progress, error)) << error;
    ASSERT_TRUE (progress.converged);
}

TEST (SunStudyRustAnnual, ScheduleCoversTheYearAndRefusesBackwardRanges)
{
    std::vector<rs::AnnualDay> days;
    uint32_t represented = 0;
    std::string error;
    ASSERT_TRUE (rs::ScheduleAnnualDays (2024, 1, 1, 12, 31, rs::AnnualSampling::Preview, days, represented, error))
        << error;
    ASSERT_EQ (days.size (), 12u);
    EXPECT_EQ (represented, 366u);
    double weight = 0.0;
    for (const auto& day : days)
        weight += day.weightDays;
    EXPECT_DOUBLE_EQ (weight, 366.0);
    EXPECT_EQ (days[1].month, 2);
    EXPECT_EQ (days[1].day, 15);
    ASSERT_TRUE (
        rs::ScheduleAnnualDays (2026, 3, 20, 3, 22, rs::AnnualSampling::DailyResolved, days, represented, error));
    ASSERT_EQ (days.size (), 3u);
    EXPECT_EQ (days.back ().day, 22);
    EXPECT_FALSE (rs::ScheduleAnnualDays (2026, 5, 2, 5, 1, rs::AnnualSampling::Preview, days, represented, error));
    EXPECT_EQ (error, "AnnualInvalidRange");
    EXPECT_FALSE (rs::ScheduleAnnualDays (2026, 2, 29, 3, 1, rs::AnnualSampling::Preview, days, represented, error));
    EXPECT_EQ (error, "AnnualInvalidDate");
}

TEST (SunStudyRustAnnual, ADayFoldsInAtItsWeightAndIsPresentedThroughItsOwnCore)
{
    const auto day = Ground ({ 1.0, 0.0, 0.0, 5.0, 0.0, 0.0 });
    ASSERT_NE (day, nullptr);
    std::string error;
    auto annual = rs::RustAnnualSun::Create (2, error);
    ASSERT_NE (annual, nullptr) << error;
    EXPECT_FALSE (annual->Add (*day, 6, 30.0, error));
    EXPECT_EQ (error, "AnnualDayNotConverged");
    Converge (*day);
    ASSERT_TRUE (annual->AddSunless (12, 31.0, error)) << error;
    ASSERT_TRUE (annual->Add (*day, 6, 30.0, error)) << error;

    std::vector<double> values;
    double ceiling = 0.0;
    ASSERT_TRUE (annual->Values (rs::AnnualQuantity::Unoccluded, 0, false, values, ceiling, error)) << error;
    EXPECT_EQ (values, (std::vector<double> { 90.0, 0.0 }));
    EXPECT_DOUBLE_EQ (ceiling, 90.0);
    ASSERT_TRUE (annual->Values (rs::AnnualQuantity::ObstructionLoss, 6, false, values, ceiling, error)) << error;
    EXPECT_EQ (values, (std::vector<double> { 0.0, 90.0 }));
    ASSERT_TRUE (annual->Values (rs::AnnualQuantity::Unoccluded, 0, true, values, ceiling, error)) << error;
    EXPECT_DOUBLE_EQ (values[0], 90.0 / 61.0);
    EXPECT_DOUBLE_EQ (ceiling, 90.0 / 61.0);

    rs::AnnualInfo info;
    ASSERT_TRUE (annual->Info (info, error)) << error;
    EXPECT_EQ (info.sampledDays, 2u);
    EXPECT_DOUBLE_EQ (info.representedDays, 61.0);
    EXPECT_DOUBLE_EQ (info.siteDaylight[5], 90.0);
    EXPECT_DOUBLE_EQ (info.monthDays[11], 31.0);
    EXPECT_DOUBLE_EQ (info.maxHours, 90.0);
    EXPECT_DOUBLE_EQ (info.ceilingHours, 90.0);

    ASSERT_TRUE (annual->Present (*day, rs::AnnualQuantity::Unoccluded, 6, true, error)) << error;
    std::vector<double> hours;
    ASSERT_TRUE (day->SunHours (hours, error)) << error;
    EXPECT_EQ (hours, (std::vector<double> { 3.0, 0.0 }));
    TapiocaStudyInfo study {};
    ASSERT_TRUE (day->Info (study, error)) << error;
    EXPECT_DOUBLE_EQ (study.daylightHours, 3.0);
    // A presented study would hand back the period, not its day.
    EXPECT_FALSE (annual->Add (*day, 6, 1.0, error));
    EXPECT_EQ (error, "AnnualDayPresented");
}

TEST (SunStudyRustAnnual, OtherSamplesAreNeverMixedIntoAPeriod)
{
    const auto first = Ground ({ 1.0, 0.0, 0.0, 5.0, 0.0, 0.0 });
    const auto moved = Ground ({ 1.0, 0.0, 0.0, 5.5, 0.0, 0.0 });
    ASSERT_NE (first, nullptr);
    ASSERT_NE (moved, nullptr);
    Converge (*first);
    Converge (*moved);
    std::string error;
    auto annual = rs::RustAnnualSun::Create (2, error);
    ASSERT_NE (annual, nullptr) << error;
    ASSERT_TRUE (annual->Add (*first, 3, 1.0, error)) << error;
    EXPECT_FALSE (annual->Add (*moved, 3, 1.0, error));
    EXPECT_EQ (error, "AnnualReceptorsChanged");
    EXPECT_FALSE (annual->Present (*moved, rs::AnnualQuantity::Unoccluded, 0, false, error));
    EXPECT_EQ (error, "AnnualReceptorsChanged");
    EXPECT_FALSE (annual->AddSunless (13, 1.0, error));
    EXPECT_FALSE (annual->AddSunless (1, 0.0, error));
    EXPECT_EQ (error, "AnnualInvalidWeight");
}

} // namespace
