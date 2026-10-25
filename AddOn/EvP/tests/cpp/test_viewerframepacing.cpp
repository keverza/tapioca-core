// Tests for ArchViz/ViewerFramePacing -- the viewer's hitch counter.

#include "ArchViz/ViewerFramePacing.hpp"
#include "gtest/gtest.h"

using geomsrv::archviz::ViewerFramePacing;

TEST (ViewerFramePacing, SteadyFramesReportNoHitch)
{
    ViewerFramePacing pacing (1.0);
    std::string line;
    for (int frame = 0; frame <= 150 && line.empty (); ++frame)
        line = pacing.Note (frame / 144.0);
    ASSERT_FALSE (line.empty ());
    EXPECT_NE (line.find ("hitches=0"), std::string::npos) << line;
}

TEST (ViewerFramePacing, ADroppedFrameIsCounted)
{
    ViewerFramePacing pacing (1.0);
    std::string line;
    double t = 0.0;
    for (int frame = 0; frame <= 200 && line.empty (); ++frame) {
        t += frame == 50 ? 3.0 / 144.0 : 1.0 / 144.0;
        line = pacing.Note (t);
    }
    ASSERT_FALSE (line.empty ());
    EXPECT_NE (line.find ("hitches=1 "), std::string::npos) << line;
}

TEST (ViewerFramePacing, ALongGapRestartsInsteadOfCountingAHitch)
{
    ViewerFramePacing pacing (1.0);
    for (int frame = 0; frame < 100; ++frame)
        EXPECT_TRUE (pacing.Note (frame / 144.0).empty ());
    std::string line;
    for (int frame = 0; frame <= 150 && line.empty (); ++frame)
        line = pacing.Note (10.0 + frame / 144.0);
    ASSERT_FALSE (line.empty ());
    EXPECT_NE (line.find ("hitches=0"), std::string::npos) << line;
}
