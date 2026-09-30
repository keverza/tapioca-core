// ArchViz/OverlayClickTiming: how long a click keeps the main thread busy, measured the
// same way for the HUD and for a DG palette -- the user's question is which is lighter,
// and a meter that counts a stretch twice, or one that is not the click's, answers it
// wrongly. Time is given in microseconds, as the input layer's hooks read it.

#include "ArchViz/OverlayClickTiming.hpp"

#include <gtest/gtest.h>

namespace clicks = geomsrv::archviz::overlayclicks;

// A press, the thread busy until it idles, busy again a little later (a paced redraw) and
// idle: both stretches are the press's, the first idle says how soon the thread was free,
// and the HUD's layout and redraw inside count apart.
TEST (OverlayClickTiming, EveryBusyStretchInThePressesHalfSecondIsItsOwn)
{
    clicks::Meter meter;
    meter.Wake (1000);                                 // something before the press
    meter.Press (clicks::Target::Hud, "Canvas", 2000); // the press: busy from here
    meter.Layout (1500, 3600);
    meter.Idle (4000);  // 2 ms
    meter.Wake (40000); // the paced redraw's timer
    meter.Redraw (3000, 43500);
    meter.Idle (44000);  // 4 ms more
    meter.Wake (600000); // past the half second: not the press's
    meter.Idle (601000);
    const std::vector<clicks::Sample> samples = meter.Samples (700000);
    ASSERT_EQ (samples.size (), 1u);
    const clicks::Sample& sample = samples[0];
    EXPECT_EQ (sample.target, clicks::Target::Hud);
    EXPECT_STREQ (sample.windowClass, "Canvas");
    EXPECT_TRUE (sample.complete);
    EXPECT_EQ (sample.busyMicroseconds, 2000u + 4000u);
    EXPECT_EQ (sample.bursts, 2u);
    EXPECT_EQ (sample.firstIdleMicroseconds, 2000u);
    EXPECT_EQ (sample.layoutMicroseconds, 1500u);
    EXPECT_EQ (sample.layouts, 1u);
    EXPECT_EQ (sample.redrawMicroseconds, 3000u);
    EXPECT_EQ (sample.redraws, 1u);
    EXPECT_EQ (meter.Idles (), 3u);
    EXPECT_STREQ (clicks::TargetName (clicks::Target::Other), "other");
}

// A stretch running over the half second's end counts up to it; a new press ends the old
// one's claim; the newest are kept, oldest first.
TEST (OverlayClickTiming, APressOwnsOnlyItsHalfSecond)
{
    clicks::Meter meter;
    meter.Press (clicks::Target::Other, "DGPalette", 0);
    meter.Idle (100);
    meter.Wake (499000);
    meter.Idle (520000); // past the end: 1 ms of it counts
    meter.Press (clicks::Target::View, "Canvas", 530000);
    EXPECT_FALSE (meter.Samples (531000).back ().complete);
    meter.Idle (531000);
    const std::vector<clicks::Sample> samples = meter.Samples (2000000);
    ASSERT_EQ (samples.size (), 2u);
    EXPECT_EQ (samples[0].busyMicroseconds, 100u + 1000u);
    EXPECT_EQ (samples[1].busyMicroseconds, 1000u);
    EXPECT_EQ (samples[1].target, clicks::Target::View);
    for (uint64_t k = 0; k < clicks::kSamples + 5; ++k)
        meter.Press (clicks::Target::Hud, "Canvas", 3000000 + k * 1000000);
    const std::vector<clicks::Sample> kept = meter.Samples (1000000000);
    ASSERT_EQ (kept.size (), clicks::kSamples);
    EXPECT_LT (kept.front ().at, kept.back ().at);
    meter.Reset ();
    EXPECT_TRUE (meter.Samples (0).empty ());
    EXPECT_EQ (meter.Idles (), 0u);
}
