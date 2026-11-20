// Tests for SunStudy/SunStudyStepAtlas -- the per-step lit bits the shadow
// views read.
//
// ⚠️ EVERY FAILURE HERE IS A PLAUSIBLE SHADOW. A bit written to the wrong texel
// puts one surface's morning on another; a range mask off by one at a word
// boundary turns "AM only" into "AM + PM" for exactly the surfaces shaded at
// the 32nd step. Both render as a perfectly reasonable picture.

#include <vector>

#include "SunStudy/SunSeries.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyStepAtlas.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

// A day-like pattern with runs, so ranges catch both lit and shadowed steps.
bool Pattern (size_t sample, size_t step)
{
    return ((sample * 7 + step * 3) % 11) < 6;
}

} // namespace

TEST (SunStudyStepAtlas, FanStepsSnapToTheClockLikeTheWebStudy)
{
    // 15-minute steps from 07:15 to 16:45.
    std::vector<uint16_t> minutes;
    for (int m = 7 * 60 + 15; m <= 16 * 60 + 45; m += 15)
        minutes.push_back (uint16_t (m));

    const std::vector<uint32_t> hourly = kernels::FanSteps (minutes, 60.0);
    ASSERT_EQ (hourly.size (), 9u) << "08:00 .. 16:00";
    EXPECT_EQ (minutes[hourly.front ()], 8 * 60);
    EXPECT_EQ (minutes[hourly.back ()], 16 * 60);

    // Every step: the study's own precision.
    EXPECT_EQ (kernels::FanSteps (minutes, 0.0).size (), minutes.size ());

    // A sparse series never reports an hour it did not sample.
    const std::vector<uint16_t> sparse { 8 * 60, 12 * 60 };
    const std::vector<uint32_t> sparsePicks = kernels::FanSteps (sparse, 60.0);
    EXPECT_EQ (sparsePicks.size (), 2u);
}
