// ArchViz/TemporalConvergence -- what the viewer does with a still camera (#14 P1, #247, #248).
//
// Every wrong answer here renders a plausible picture: a mean that never restarts shows a stale
// image after a change, a mean that starts during motion averages in a trail, and a jitter that
// never stops leaves the overlays drawn after the resolve shimmering on a converged image. So the
// phase sequence and the moment each restart happens are pinned here.

#include "ArchViz/TemporalConvergence.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

struct Frame {
    float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -5, 1 };
    float proj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -0.1f, 0 };
    uint32_t width = 640, height = 480;
};

TemporalConvergenceSettings Small ()
{
    TemporalConvergenceSettings s;
    s.settleFrames = 2;
    s.targetSamples = 4;
    return s;
}

// One frame: begin, then end with the content key, returning what to do with the mean.
TemporalStep Step (TemporalConvergence& t, const Frame& f, uint64_t content = 7, bool resolved = true,
                   const TemporalConvergenceSettings& s = Small (), bool running = true)
{
    t.BeginFrame (f.view, f.proj, f.width, f.height, running, s);
    return t.EndFrame (content, resolved);
}

} // namespace

TEST (TemporalConvergence, StillCameraSettlesThenAccumulatesThenConverges)
{
    TemporalConvergence t;
    Frame f;
    // First frame: nothing to compare with, so it counts as motion.
    EXPECT_FALSE (Step (t, f).accumulate);
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
    // Two settling frames present TAA as before.
    for (int i = 0; i < 2; ++i) {
        const TemporalStep step = Step (t, f);
        EXPECT_FALSE (step.accumulate);
        EXPECT_FALSE (step.present);
        EXPECT_EQ (t.Phase (), TemporalPhase::Settling);
        EXPECT_TRUE (t.JitterThisFrame ());
    }
    // Four samples, each weighted as a running mean: 1, 1/2, 1/3, 1/4.
    for (int k = 1; k <= 4; ++k) {
        const TemporalStep step = Step (t, f);
        EXPECT_TRUE (step.accumulate);
        EXPECT_TRUE (step.present);
        EXPECT_FLOAT_EQ (step.weight, 1.0f / float (k));
        EXPECT_EQ (t.Samples (), uint32_t (k));
    }
    EXPECT_EQ (t.Phase (), TemporalPhase::Converged);
    // Converged: the mean is shown, never blended again, and the jitter is off.
    EXPECT_FALSE (t.JitterThisFrame ());
    const TemporalStep held = Step (t, f);
    EXPECT_FALSE (held.accumulate);
    EXPECT_TRUE (held.present);
    EXPECT_FALSE (t.JitterThisFrame ());
}

TEST (TemporalConvergence, FirstSampleOverwritesTheMean)
{
    // Weight 1 on the first sample is what makes a stale mean from an earlier view harmless: the
    // accumulator never needs clearing, and a mean that started from garbage would show it.
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 3; ++i)
        Step (t, f);
    const TemporalStep first = Step (t, f);
    EXPECT_TRUE (first.accumulate);
    EXPECT_FLOAT_EQ (first.weight, 1.0f);
}

TEST (TemporalConvergence, CameraMoveRestartsAndJittersThatSameFrame)
{
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 10; ++i)
        Step (t, f);
    ASSERT_EQ (t.Phase (), TemporalPhase::Converged);

    // The jitter for a frame is chosen between BeginFrame and EndFrame, so a move must be seen at
    // BeginFrame: a converged view that starts moving is jittered on its first moving frame.
    f.view[12] += 0.001f;
    t.BeginFrame (f.view, f.proj, f.width, f.height, true, Small ());
    EXPECT_TRUE (t.JitterThisFrame ());
    const TemporalStep step = t.EndFrame (7, true);
    EXPECT_FALSE (step.present);
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
    EXPECT_EQ (t.Samples (), 0u);
}

TEST (TemporalConvergence, ProjectionAndViewportSizeAreTheCamera)
{
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 10; ++i)
        Step (t, f);
    f.proj[0] = 1.1f; // a zoom
    Step (t, f);
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);

    for (int i = 0; i < 10; ++i)
        Step (t, f);
    f.width = 641; // a resize
    Step (t, f);
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
}

TEST (TemporalConvergence, ContentChangeRestartsEvenWhenConverged)
{
    // The case a missing fingerprint input would break: a setting changed, the camera did not.
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 10; ++i)
        Step (t, f, 7);
    ASSERT_EQ (t.Phase (), TemporalPhase::Converged);
    const TemporalStep step = Step (t, f, 8);
    EXPECT_FALSE (step.present);
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
    EXPECT_TRUE (t.JitterThisFrame ());
}

TEST (TemporalConvergence, UnresolvedTaaNeverAccumulates)
{
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 20; ++i) {
        const TemporalStep step = Step (t, f, 7, false);
        EXPECT_FALSE (step.accumulate);
        EXPECT_FALSE (step.present);
    }
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
}

TEST (TemporalConvergence, NotRunningIsOffAndRestartsFromMoving)
{
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 10; ++i)
        Step (t, f);
    ASSERT_EQ (t.Phase (), TemporalPhase::Converged);

    const TemporalStep off = Step (t, f, 7, true, Small (), false);
    EXPECT_FALSE (off.present);
    EXPECT_EQ (t.Phase (), TemporalPhase::Off);
    EXPECT_TRUE (t.JitterThisFrame ());

    // Back on with the same camera: the mean is not trusted across the gap.
    Step (t, f);
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
}

TEST (TemporalConvergence, DisabledSettingIsOff)
{
    TemporalConvergence t;
    Frame f;
    TemporalConvergenceSettings s = Small ();
    s.enabled = false;
    for (int i = 0; i < 10; ++i) {
        const TemporalStep step = Step (t, f, 7, true, s);
        EXPECT_FALSE (step.accumulate);
        EXPECT_FALSE (step.present);
    }
    EXPECT_EQ (t.Phase (), TemporalPhase::Off);
}

TEST (TemporalConvergence, ResetRestartsEvenWithAnUnchangedCamera)
{
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 10; ++i)
        Step (t, f);
    ASSERT_EQ (t.Phase (), TemporalPhase::Converged);
    t.Reset ();
    EXPECT_TRUE (t.JitterThisFrame ());
    Step (t, f);
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
}

TEST (TemporalConvergenceKey, OrderAndSignedZero)
{
    ContentKey a, b, c, d;
    a.Add (1.0f);
    a.Add (2.0f);
    b.Add (2.0f);
    b.Add (1.0f);
    EXPECT_NE (a.Value (), b.Value ());

    c.Add (0.0f);
    d.Add (-0.0f);
    EXPECT_EQ (c.Value (), d.Value ());
}

// #247 live: "stable for a few seconds, then jitters". The readout names what threw a still image
// back to Moving and counts only interruptions, so a view that restarts on its own says why.
TEST (TemporalConvergence, RestartsAreCountedWithTheirReason)
{
    TemporalConvergence t;
    Frame f;
    for (int i = 0; i < 8; ++i)
        Step (t, f);
    ASSERT_EQ (t.Phase (), TemporalPhase::Converged);
    EXPECT_EQ (t.Status ().restarts, 0u);
    EXPECT_EQ (t.Status ().lastRestart, TemporalRestart::None);

    Step (t, f, 8); // content changed while converged
    EXPECT_EQ (t.Status ().restarts, 1u);
    EXPECT_EQ (t.Status ().lastRestart, TemporalRestart::Content);

    // Moving -> Moving is the camera still moving, not another interruption.
    Frame moved = f;
    moved.view[12] = 1.0f;
    Step (t, moved, 8);
    EXPECT_EQ (t.Status ().restarts, 1u);

    for (int i = 0; i < 3; ++i)
        Step (t, moved, 8);
    ASSERT_EQ (t.Phase (), TemporalPhase::Accumulating);
    Step (t, f, 8); // the camera moves during accumulation
    EXPECT_EQ (t.Status ().restarts, 2u);
    EXPECT_EQ (t.Status ().lastRestart, TemporalRestart::Camera);

    for (int i = 0; i < 3; ++i)
        Step (t, f, 8);
    Step (t, f, 8, false); // TAA resolved nothing
    EXPECT_EQ (t.Status ().lastRestart, TemporalRestart::Unresolved);

    for (int i = 0; i < 3; ++i)
        Step (t, f, 8);
    t.Reset (TemporalRestart::Content); // the caller found the change (an idle frame)
    EXPECT_EQ (t.Phase (), TemporalPhase::Moving);
    EXPECT_EQ (t.Status ().restarts, 4u);
    EXPECT_EQ (t.Status ().lastRestart, TemporalRestart::Content);
}

// #14: the log line that names what keeps a still image from settling in Archicad.
TEST (TemporalActivityLog, CountsPhasesAndRestartsByReasonPerWindow)
{
    TemporalConvergence t;
    TemporalActivityLog log (1.0);
    Frame f;
    double now = 0.0;
    std::string line;
    auto frame = [&] (const Frame& at) {
        t.BeginFrame (at.view, at.proj, at.width, at.height, true, Small ());
        const std::string l = log.Note (t.Status (), false, now);
        if (!l.empty ())
            line = l;
        t.EndFrame (7, true);
        now += 0.1;
    };
    for (int i = 0; i < 4; ++i)
        frame (f); // moving, then settling
    Frame moved = f;
    moved.view[12] = 1.0f;
    frame (moved); // a camera change interrupts the still image
    for (int i = 0; i < 4; ++i)
        frame (moved);
    t.Reset ();
    for (int i = 0; i < 3; ++i)
        frame (moved);
    ASSERT_FALSE (line.empty ());
    EXPECT_NE (line.find ("by camera 1, content 0, unresolved 0, reset 1"), std::string::npos) << line;

    // A window with nothing temporal running says nothing.
    TemporalActivityLog quiet (1.0);
    TemporalStatus off;
    std::string said;
    for (int i = 0; i < 30; ++i)
        said += quiet.Note (off, false, i * 0.1);
    EXPECT_TRUE (said.empty ());
}

// #14 moire: the still image's samples are spread over a tent (TemporalConvergenceSettings::
// filterRadiusPixels). Measured on RenderBench louvres, geometry only: the louvre crop's error
// against a 4x Lanczos truth 2.08 -> 1.62 (Architecture) and 3.22 -> 2.34 (Ray traced) at radius 1.
TEST (TemporalConvergence, TentOffsetsSpanTheRadiusMonotonicAndSymmetric)
{
    using geomsrv::archviz::TentOffsetPixels;
    EXPECT_FLOAT_EQ (TentOffsetPixels (0.25f, 0.5f), -0.25f) << "radius 0.5 is the old in-pixel box";
    EXPECT_FLOAT_EQ (TentOffsetPixels (0.0f, 1.0f), -1.0f);
    EXPECT_FLOAT_EQ (TentOffsetPixels (0.5f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ (TentOffsetPixels (1.0f, 1.0f), 1.0f);
    float previous = -2.0f;
    for (int i = 0; i <= 100; ++i) {
        const float u = float (i) / 100.0f;
        const float x = TentOffsetPixels (u, 1.0f);
        EXPECT_GE (x, previous) << "monotonic, so a stratified Halton stays stratified";
        EXPECT_NEAR (x, -TentOffsetPixels (1.0f - u, 1.0f), 1e-5f);
        previous = x;
    }
    // Half the samples within 1 - sqrt(1/2) of the centre: the tent's median distance.
    EXPECT_NEAR (TentOffsetPixels (0.75f, 1.0f), 1.0f - std::sqrt (0.5f), 1e-5f);
}
