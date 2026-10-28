#include "ArchViz/AnalysisTabs.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz::analysistabs;

TEST (AnalysisTabs, ClearingTheDisplayNeverMovesTheFocus)
{
    Focus focus;
    Opened (focus, kSurfaceVisibilityTab);
    FollowShownStudy (focus, "visibility-viewer-1", 1, 2, 0);
    EXPECT_EQ (focus.tab, kSurfaceVisibilityTab);
    // A role edit clears the tint: the kind falls back to 0 and nothing is shown.
    FollowShownStudy (focus, "", 0, 3, 0);
    Opened (focus, kSurfaceVisibilityTab);
    EXPECT_EQ (focus.tab, kSurfaceVisibilityTab);
    EXPECT_EQ (focus.selectTab, -1);
}

TEST (AnalysisTabs, AnExplicitlyClaimedStudyTakesTheFocusToItsTab)
{
    Focus focus;
    Opened (focus, kViewpointTab);
    FollowShownStudy (focus, "visibility-7", 1, 4, 1);
    EXPECT_EQ (focus.tab, kPointVisibilityTab);
    EXPECT_EQ (focus.selectTab, kPointVisibilityTab);
    // The frame before the tab bar switches still reports the old tab open.
    Opened (focus, kViewpointTab);
    EXPECT_EQ (focus.tab, kPointVisibilityTab);
    Opened (focus, kPointVisibilityTab);
    EXPECT_EQ (focus.selectTab, -1);
}

TEST (AnalysisTabs, ABackgroundSunRefreshDoesNotStealTheFocus)
{
    Focus focus;
    FollowShownStudy (focus, "sun-1", 0, 5, 0);
    Opened (focus, kSunTab);
    Opened (focus, kViewpointTab);
    // Same claim generation: the follower replaced the atlas without claiming.
    FollowShownStudy (focus, "sun-2", 0, 5, 0);
    EXPECT_EQ (focus.tab, kViewpointTab);
    EXPECT_FALSE (ShowsTint (focus, "sun-2", 0));
}

TEST (AnalysisTabs, OnlyTheOwningTabShowsTheTint)
{
    Focus focus;
    FollowShownStudy (focus, "visibility-viewer-2", 1, 1, 0);
    EXPECT_TRUE (ShowsTint (focus, "visibility-viewer-2", 1));
    Opened (focus, kSurfaceVisibilityTab); // the tab bar honours the selection
    Opened (focus, kPointVisibilityTab);
    EXPECT_FALSE (ShowsTint (focus, "visibility-viewer-2", 1)) << "a surface result is not the point tab's";
    Opened (focus, kSunTab);
    EXPECT_FALSE (ShowsTint (focus, "visibility-viewer-2", 1));
    Opened (focus, kSurfaceVisibilityTab);
    EXPECT_TRUE (ShowsTint (focus, "visibility-viewer-2", 1));
    EXPECT_TRUE (ShowsTint (focus, "", 0)) << "nothing resident, nothing to hide";
}
