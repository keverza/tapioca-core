// Tests for SunStudy/SunStudyStepAtlas -- the per-step lit bits the shadow
// views read.
//
// ⚠️ EVERY FAILURE HERE IS A PLAUSIBLE SHADOW. A bit written to the wrong texel
// puts one surface's morning on another; a range mask off by one at a word
// boundary turns "AM only" into "AM + PM" for exactly the surfaces shaded at
// the 32nd step. Both render as a perfectly reasonable picture.

#include <vector>

#include "SunStudy/SunSeries.hpp"
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

TEST (SunStudyStepAtlas, EverySampleDayLandsOnItsOwnTexel)
{
    const uint32_t width = 8;
    const uint32_t height = 4;
    const size_t samples = 20;
    const size_t steps = 70; // three words
    // A scrambled placement, and one sample with no texel.
    std::vector<int64_t> texelOf (samples);
    for (size_t i = 0; i < samples; ++i)
        texelOf[i] = static_cast<int64_t> ((i * 13 + 5) % (width * height));
    texelOf[7] = -1;

    const StepMaskAtlas atlas = PackStepMasks (samples, steps, Pattern, texelOf, width, height);
    EXPECT_EQ (atlas.words, 3u);
    EXPECT_EQ (atlas.masks.size (), size_t (width) * height * 3);

    for (size_t sample = 0; sample < samples; ++sample) {
        if (texelOf[sample] < 0)
            continue;
        for (size_t step = 0; step < steps; ++step)
            ASSERT_EQ (atlas.Lit (size_t (texelOf[sample]), step), Pattern (sample, step))
                << "sample " << sample << " step " << step;
    }
}

TEST (SunStudyStepAtlas, ShadowedBetweenAgreesWithAStepByStepAnswerOverEveryRange)
{
    // ⚠️ EVERY [first, last) PAIR, so both word boundaries (32, 64) are crossed,
    // started on, and ended on.
    const size_t steps = 70;
    const std::vector<int64_t> texelOf { 0, 1, 2 };
    const StepMaskAtlas atlas = PackStepMasks (3, steps, Pattern, texelOf, 3, 1);

    for (size_t texel = 0; texel < 3; ++texel) {
        for (uint32_t first = 0; first <= steps; ++first) {
            for (uint32_t last = first; last <= steps; ++last) {
                bool expected = false;
                for (uint32_t step = first; step < last; ++step)
                    expected = expected || !Pattern (texel, step);
                ASSERT_EQ (atlas.ShadowedBetween (texel, first, last), expected)
                    << "texel " << texel << " [" << first << ", " << last << ")";
            }
        }
    }
}

TEST (SunStudyStepAtlas, AStudyOfNoStepsStillHasOneSlice)
{
    // The shader binds the texture on every draw; a zero-slice array is not a
    // texture at all.
    const StepMaskAtlas atlas = PackStepMasks (0, 0, Pattern, {}, 4, 4);
    EXPECT_EQ (atlas.words, 1u);
    EXPECT_EQ (atlas.masks.size (), 16u);
}

TEST (SunStudyStepAtlas, SolarNoonIsTheHighestSunNotTwelveOClock)
{
    std::vector<SunStep> raw;
    const double altitudes[5] = { 5.0, 20.0, 30.0, 25.0, 10.0 };
    for (int i = 0; i < 5; ++i) {
        SunStep step;
        step.time.hour = 10 + i; // 10:00 .. 14:00: clock noon is step 2 too...
        step.altitudeDegrees = altitudes[i];
        raw.push_back (step);
    }
    raw[2].altitudeDegrees = 24.0; // ...so move the peak off it: step 3, 13:00
    const SunSeries series = SunSeries::FromSteps (raw, 60);
    EXPECT_EQ (SolarNoonStep (series), 3u);
    const std::vector<uint16_t> minutes = StepMinutes (series);
    ASSERT_EQ (minutes.size (), 5u);
    EXPECT_EQ (minutes[0], 600u);
    EXPECT_EQ (minutes[4], 840u);
}

TEST (SunStudyStepAtlas, FanStepsSnapToTheClockLikeTheWebStudy)
{
    // 15-minute steps from 07:15 to 16:45.
    std::vector<uint16_t> minutes;
    for (int m = 7 * 60 + 15; m <= 16 * 60 + 45; m += 15)
        minutes.push_back (uint16_t (m));

    const std::vector<uint32_t> hourly = FanSteps (minutes, 60.0);
    ASSERT_EQ (hourly.size (), 9u) << "08:00 .. 16:00";
    EXPECT_EQ (minutes[hourly.front ()], 8 * 60);
    EXPECT_EQ (minutes[hourly.back ()], 16 * 60);

    // Every step: the study's own precision.
    EXPECT_EQ (FanSteps (minutes, 0.0).size (), minutes.size ());

    // A sparse series never reports an hour it did not sample.
    const std::vector<uint16_t> sparse { 8 * 60, 12 * 60 };
    const std::vector<uint32_t> sparsePicks = FanSteps (sparse, 60.0);
    EXPECT_EQ (sparsePicks.size (), 2u);
}

TEST (SunStudyStepAtlas, FanRankIsTheLastShadowedSelectedStepAgainstABruteForceAnswer)
{
    const size_t steps = 90;
    const std::vector<int64_t> texelOf { 0, 1, 2, 3 };
    const StepMaskAtlas atlas = PackStepMasks (4, steps, Pattern, texelOf, 4, 1);

    // Several selections, including ones straddling both word boundaries.
    const std::vector<std::vector<uint32_t>> selections {
        { 0, 5, 31, 32, 33, 63, 64, 89 }, { 10, 40, 70 }, { 3 }, {}, { 30, 31, 32, 62, 63, 64, 65 }
    };
    for (const std::vector<uint32_t>& selection : selections) {
        uint32_t mask[3] = { 0u, 0u, 0u };
        for (const uint32_t step : selection)
            mask[step >> 5] |= 1u << (step & 31u);
        for (size_t texel = 0; texel < 4; ++texel) {
            int expected = -1;
            for (size_t rank = 0; rank < selection.size (); ++rank)
                if (!Pattern (texel, selection[rank]))
                    expected = int (rank); // selection is ascending: the last wins
            EXPECT_EQ (atlas.FanRank (texel, mask), expected) << "texel " << texel;
        }
    }
}
