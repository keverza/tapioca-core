// ArchViz/Dxgi/CameraChoice -- which reading of a group's pair wins, and which
// eligible group becomes the camera.
//
// WHAT THESE TESTS ARE FOR. Both decisions cost live runs. On 2026-09-27 at
// 16:53:10 the calibrated selection chose reading 7 on a spread spike, the
// injection refused to draw a reversed order, and the overlay vanished a second
// after it had appeared. And once `b2` decodes in both layouts every
// camera-bearing draw of a frame is eligible -- including a 24-index helper whose
// camera is the previous image's. The numbers below are the census's
// own, as logged.

#include "ArchViz/Dxgi/CameraChoice.hpp"

#include <gtest/gtest.h>

namespace cc = geomsrv::archviz::dxgi::camerachoice;

namespace {

// The rule as it was: every reading may win.
uint32_t WinnerAmongAllEight (const uint32_t valid[], const float spread[], const float error[])
{
    uint32_t best = 0, bestValid = 0;
    float bestSpread = 0.0f, bestError = 0.0f;
    for (uint32_t v = 0; v < 8; ++v) {
        if (valid[v] == 0)
            continue;
        if (valid[v] > bestValid || (valid[v] == bestValid && spread[v] > bestSpread) ||
            (valid[v] == bestValid && spread[v] == bestSpread && error[v] < bestError)) {
            best = v;
            bestValid = valid[v];
            bestSpread = spread[v];
            bestError = error[v];
        }
    }
    return best;
}

cc::Standing At (float coverage, float insideClip, float error, uint32_t variant, uint32_t indexCount)
{
    cc::Standing s;
    s.coverage = coverage;
    s.insideClip = insideClip;
    s.centreError = error;
    s.variant = variant;
    s.indexCount = indexCount;
    return s;
}

} // namespace

// 16:53:10, group g4 (the editing plane's quad), at the calibrated decision.
TEST (CameraChoice, TheTieThatHidTheOverlayGoesToADrawableReading)
{
    const uint32_t valid[8] = { 95, 0, 9, 0, 0, 46, 0, 95 };
    const float spread[8] = { 225.0f, 0.0f, 28.0f, 0.0f, 0.0f, 60.0f, 0.0f, 311.0f };
    const float error[8] = { 0.274f, 0.0f, 0.030f, 0.0f, 0.0f, 0.077f, 0.0f, 0.091f };
    ASSERT_EQ (WinnerAmongAllEight (valid, spread, error), 7u) << "what the census chose, and could not draw";
    uint32_t winnerValid = 0;
    EXPECT_EQ (cc::WinningVariant (valid, spread, error, winnerValid), 0u);
    EXPECT_EQ (winnerValid, 95u);
}

// Finding 12's own measurement, 2026-09-19 17:19 g9: the spread tie still decides.
TEST (CameraChoice, FindingTwelvesMeasurementStillGoesToReadingZero)
{
    const uint32_t valid[8] = { 565, 0, 468, 0, 0, 532, 0, 565 };
    const float spread[8] = { 261.0f, 0.0f, 68.0f, 0.0f, 0.0f, 33.0f, 0.0f, 59.0f };
    const float error[8] = { 0.194f, 0.0f, 0.052f, 0.0f, 0.0f, 0.033f, 0.0f, 0.051f };
    uint32_t winnerValid = 0;
    EXPECT_EQ (cc::WinningVariant (valid, spread, error, winnerValid), 0u);
    EXPECT_EQ (winnerValid, 565u);
}

TEST (CameraChoice, AGroupWithNoValidDrawableReadingHasNoWinner)
{
    const uint32_t valid[8] = { 0, 0, 0, 0, 12, 0, 0, 40 };
    const float spread[8] = {};
    const float error[8] = {};
    uint32_t winnerValid = 99;
    EXPECT_EQ (cc::WinningVariant (valid, spread, error, winnerValid), 0u);
    EXPECT_EQ (winnerValid, 0u) << "a reversed order never counts as valid for the winner";
}

TEST (CameraChoice, TheModelsLargestDrawOutranksAHelper)
{
    const cc::Standing helper = At (1.0f, 1.0f, 0.05f, 0, 24); // the previous image's camera
    const cc::Standing model = At (1.0f, 1.0f, 0.07f, 0, 1512);
    EXPECT_TRUE (cc::Outranks (model, &helper)) << "more geometry wins even at a larger centre error";
    EXPECT_FALSE (cc::Outranks (helper, &model));
}

TEST (CameraChoice, TheEditingPlaneLosesToTheModel)
{
    const cc::Standing plane = At (1.0f, 1.0f, 0.07f, 0, 6);
    const cc::Standing model = At (0.995f, 1.0f, 0.08f, 0, 408); // within the 1% coverage tie
    EXPECT_TRUE (cc::Outranks (model, &plane));
    EXPECT_FALSE (cc::Outranks (plane, &model));
}

TEST (CameraChoice, CoverageAndClipStillComeFirst)
{
    const cc::Standing sparse = At (0.85f, 1.0f, 0.05f, 0, 1512);
    const cc::Standing helper = At (1.0f, 1.0f, 0.05f, 0, 24);
    EXPECT_TRUE (cc::Outranks (helper, &sparse)) << "a camera seen in fewer frames is worse whatever it draws";
    const cc::Standing clipped = At (1.0f, 0.95f, 0.05f, 0, 1512);
    const cc::Standing inside = At (1.0f, 1.0f, 0.05f, 0, 24);
    EXPECT_TRUE (cc::Outranks (inside, &clipped));
}

TEST (CameraChoice, TheCanonicalReadingStillBeatsGeometry)
{
    const cc::Standing transposed = At (1.0f, 1.0f, 0.05f, 2, 1512);
    const cc::Standing canonical = At (1.0f, 1.0f, 0.05f, 0, 24);
    EXPECT_TRUE (cc::Outranks (canonical, &transposed));
    EXPECT_FALSE (cc::Outranks (transposed, &canonical));
}

TEST (CameraChoice, AtEqualGeometryTheSmallerErrorWins)
{
    const cc::Standing held = At (1.0f, 1.0f, 0.05f, 0, 1512);
    EXPECT_TRUE (cc::Outranks (At (1.0f, 1.0f, 0.04f, 0, 1512), &held));
    EXPECT_FALSE (cc::Outranks (At (1.0f, 1.0f, 0.06f, 0, 1512), &held));
    EXPECT_TRUE (cc::Outranks (At (0.0f, 0.0f, 1.0f, 0, 0), nullptr)) << "anything outranks nothing";
}

// 17:24:31, plane hidden: the first generation after learning numbered 2D
// screen-map draws at the model's occurrences, and 9 of 10 groups were refused.
TEST (CameraChoice, AScreenMapFromAnotherDrawDoesNotSpeakForTheGroup)
{
    EXPECT_FALSE (cc::CountsForGroup (false, 6, 1512)) << "a UI quad at the 1512-index draw's occurrence";
    EXPECT_TRUE (cc::CountsForGroup (true, 1512, 1512));
    EXPECT_TRUE (cc::CountsForGroup (true, 1530, 1512)) << "a camera sample always counts -- the model after an edit";
    EXPECT_TRUE (cc::CountsForGroup (false, 1512, 1512)) << "the camera draw itself without a camera: refused, as before";
}

TEST (CameraChoice, NothingCountsBeforeTheFirstCameraSample)
{
    EXPECT_FALSE (cc::CountsForGroup (false, 6, 0)) << "a group of screen maps alone never accumulates";
    EXPECT_TRUE (cc::CountsForGroup (true, 24, 0));
}

TEST (CameraChoice, TheSnapshotIsTakenOnlyFromTheCameraDraw)
{
    EXPECT_TRUE (cc::IsCameraDraw (1512, 1512));
    EXPECT_FALSE (cc::IsCameraDraw (6, 1512)) << "a screen-map draw that took the pinned occurrence";
    EXPECT_TRUE (cc::IsCameraDraw (6, 0)) << "no camera sample yet: nothing to refuse on";
}
