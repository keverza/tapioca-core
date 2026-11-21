#include "ArchViz/FrameFlash.hpp"

#include <gtest/gtest.h>

using geomsrv::archviz::FrameFlashDetector;

// #14: a single white frame among ordinary ones is reported once the next frame is back.
TEST (FrameFlash, ASingleWhiteFrameIsReportedWhenThePictureComesBack)
{
    FrameFlashDetector d;
    for (uint64_t f = 0; f < 10; ++f)
        EXPECT_FALSE (d.Add (f, 172.0f + float (f % 3)).flash);
    EXPECT_FALSE (d.Add (10, 240.0f).flash); // held, not known yet
    const FrameFlashDetector::Result r = d.Add (11, 173.0f);
    ASSERT_TRUE (r.flash);
    EXPECT_EQ (r.first, 10u);
    EXPECT_EQ (r.frames, 1u);
    EXPECT_NEAR (r.peak, 240.0f, 2.0f);
    EXPECT_NEAR (r.baseline, 173.0f, 2.0f);
}

TEST (FrameFlash, TwoFramesAreStillAFlash)
{
    FrameFlashDetector d;
    for (uint64_t f = 0; f < 5; ++f)
        d.Add (f, 180.0f);
    EXPECT_FALSE (d.Add (5, 235.0f).flash);
    EXPECT_FALSE (d.Add (6, 230.0f).flash);
    const FrameFlashDetector::Result r = d.Add (7, 181.0f);
    ASSERT_TRUE (r.flash);
    EXPECT_EQ (r.first, 5u);
    EXPECT_EQ (r.frames, 2u);
    EXPECT_NEAR (r.peak, 235.0f, 0.5f);
}

// A style switch changes the level for good: it becomes the baseline and is never reported.
TEST (FrameFlash, ALastingChangeIsTheNewPictureNotAFlash)
{
    FrameFlashDetector d;
    for (uint64_t f = 0; f < 5; ++f)
        d.Add (f, 150.0f);
    for (uint64_t f = 5; f < 20; ++f)
        EXPECT_FALSE (d.Add (f, 205.0f).flash) << f;
    EXPECT_NEAR (d.baseline, 205.0f, 1.0f);
}

TEST (FrameFlash, GradualChangesFollowTheBaseline)
{
    FrameFlashDetector d;
    float level = 120.0f;
    for (uint64_t f = 0; f < 100; ++f, level += 1.0f) // a slow pan from dark to light
        EXPECT_FALSE (d.Add (f, level).flash) << f;
}
