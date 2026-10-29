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

TEST (AnalysisTabs, EachTintTabPutsItsOwnStudyBackOncePerVisit)
{
    Focus focus;
    FollowShownStudy (focus, "sun-1", 0, 1, 0);
    Opened (focus, kSunTab);
    EXPECT_EQ (KeepShownStudy (focus, "sun-1", 0), -1);
    // A visibility run evicts the sun study from the shared channel.
    FollowShownStudy (focus, "visibility-viewer-1", 1, 2, 0);
    Opened (focus, kSurfaceVisibilityTab);
    EXPECT_EQ (KeepShownStudy (focus, "visibility-viewer-1", 1), -1);
    // Back on the sun tab: put the sun study back, and ask only once.
    Opened (focus, kSunTab);
    EXPECT_EQ (KeepShownStudy (focus, "visibility-viewer-1", 1), kSunTab);
    EXPECT_EQ (KeepShownStudy (focus, "visibility-viewer-1", 1), -1) << "one ask per visit";
    // It arrives under a claim the HUD marked seen: the focus stays.
    focus.seenClaim = 3;
    FollowShownStudy (focus, "sun-1", 0, 3, 0);
    Opened (focus, kSunTab);
    EXPECT_EQ (focus.tab, kSunTab);
    EXPECT_EQ (focus.selectTab, -1);
    EXPECT_EQ (KeepShownStudy (focus, "sun-1", 0), -1);
    // And the visibility tab puts its own back in turn.
    Opened (focus, kSurfaceVisibilityTab);
    EXPECT_EQ (KeepShownStudy (focus, "sun-1", 0), kSurfaceVisibilityTab);
}

TEST (AnalysisTabs, APutBackVisibilityStudyKeepsItsOwnTabsOrigin)
{
    Focus focus;
    focus.keptStudy[kSurfaceVisibilityTab] = "visibility-viewer-1";
    focus.keptStudy[kPointVisibilityTab] = "visibility-viewer-2";
    Opened (focus, kSurfaceVisibilityTab);
    // The last run was the point tab's, but this is the surface tab's study.
    FollowShownStudy (focus, "visibility-viewer-1", 1, 4, 1);
    EXPECT_EQ (focus.shownOrigin, 0);
    EXPECT_TRUE (ShowsTint (focus, "visibility-viewer-1", 1));
}

TEST (AnalysisTabs, AStudyClearedOnItsOwnTabIsNotPutBack)
{
    Focus focus;
    FollowShownStudy (focus, "visibility-viewer-1", 1, 1, 0);
    Opened (focus, kSurfaceVisibilityTab);
    KeepShownStudy (focus, "visibility-viewer-1", 1);
    FollowShownStudy (focus, "", 0, 2, 0);
    KeepShownStudy (focus, "", 0);
    EXPECT_TRUE (focus.keptStudy[kSurfaceVisibilityTab].empty ());
    Opened (focus, kSunTab);
    KeepShownStudy (focus, "", 0);
    Opened (focus, kSurfaceVisibilityTab);
    EXPECT_EQ (KeepShownStudy (focus, "", 0), -1);
}
