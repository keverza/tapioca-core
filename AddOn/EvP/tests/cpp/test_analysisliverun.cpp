#include "ArchViz/AnalysisLiveRerun.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz::liverun;

TEST (AnalysisLiveRerun, NothingFollowsUntilTheFirstRun)
{
    Follow follow;
    EXPECT_EQ (Decide (follow, "capture-1|a", false, 0.0, 0.3), Step::Idle);
    EXPECT_EQ (Decide (follow, "capture-1|a", false, 10.0, 0.3), Step::Idle);
}

TEST (AnalysisLiveRerun, AModelEditRunsAgainOnceTheCaptureSettles)
{
    Follow follow;
    Submitted (follow, "capture-1|a");
    EXPECT_EQ (Decide (follow, "capture-1|a", false, 1.0, 0.3), Step::Idle);
    // The edit makes the capture stale: the inputs cannot run, the result stays.
    EXPECT_EQ (Decide (follow, "", false, 2.0, 0.3), Step::Wait);
    // The new capture arrives and is quiet for the quiet period.
    EXPECT_EQ (Decide (follow, "capture-2|a", false, 3.0, 0.3), Step::Wait);
    EXPECT_EQ (Decide (follow, "capture-2|a", false, 3.2, 0.3), Step::Wait);
    EXPECT_EQ (Decide (follow, "capture-2|a", false, 3.35, 0.3), Step::Submit);
    Submitted (follow, "capture-2|a");
    EXPECT_EQ (Decide (follow, "capture-2|a", true, 3.4, 0.3), Step::Idle);
}

TEST (AnalysisLiveRerun, ADragRunsOnceAtItsEnd)
{
    Follow follow;
    Submitted (follow, "grid=2.0");
    double now = 0.0;
    for (int step = 1; step <= 20; ++step, now += 1.0 / 60.0)
        EXPECT_EQ (Decide (follow, "grid=" + std::to_string (2.0 + step * 0.1), false, now, 0.3), Step::Wait);
    const std::string last = "grid=released";
    EXPECT_EQ (Decide (follow, last, false, now, 0.3), Step::Wait);
    EXPECT_EQ (Decide (follow, last, false, now + 0.29, 0.3), Step::Wait);
    EXPECT_EQ (Decide (follow, last, false, now + 0.31, 0.3), Step::Submit);
}

TEST (AnalysisLiveRerun, ARunInFlightIsCancelledBeforeTheNewInputsRun)
{
    Follow follow;
    Submitted (follow, "a");
    EXPECT_EQ (Decide (follow, "b", true, 0.0, 0.0), Step::Cancel);
    EXPECT_EQ (Decide (follow, "b", true, 0.1, 0.0), Step::Cancel);
    EXPECT_EQ (Decide (follow, "b", false, 0.2, 0.0), Step::Submit);
}

TEST (AnalysisLiveRerun, GoingBackToTheSubmittedInputsCancelsNothing)
{
    Follow follow;
    Submitted (follow, "a");
    EXPECT_EQ (Decide (follow, "b", false, 0.0, 0.3), Step::Wait);
    EXPECT_EQ (Decide (follow, "a", false, 0.1, 0.3), Step::Idle);
    // The settle clock restarts for a new change; "b" is not remembered as ready.
    EXPECT_EQ (Decide (follow, "b", false, 0.5, 0.3), Step::Wait);
}

TEST (AnalysisLiveRerun, AClearedResultStopsFollowing)
{
    Follow follow;
    Submitted (follow, "a");
    Disarm (follow);
    EXPECT_EQ (Decide (follow, "b", false, 5.0, 0.0), Step::Idle);
    EXPECT_FALSE (follow.armed);
}
