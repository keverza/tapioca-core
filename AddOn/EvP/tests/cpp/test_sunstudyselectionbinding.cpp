#include "SunStudy/SunStudySelectionBinding.hpp"
#include "SunStudy/SunStudyFollower.hpp"

#include <gtest/gtest.h>

using namespace evp::sunstudy;

TEST (SunStudySelectionBinding, RemovingLastContextEntryReturnsItToAnalysis)
{
    SunStudySelectionBinding binding { "Context", "Ignored", 8, 10 };
    std::vector<std::string> context { "site" }, ignored { "excluded" };
    ASSERT_EQ (binding.Refresh (8, {}, ignored, context, ignored), SelectionBindingRefresh::Changed);
    EXPECT_TRUE (context.empty ());
    const auto roles = ResolveElementRoles ({ "site", "excluded", "new" }, {}, context, ignored);
    EXPECT_EQ (roles.SampleMask (), (std::vector<uint8_t> { 1, 0, 1 }));
    EXPECT_EQ (binding.Refresh (8, {}, ignored, context, ignored), SelectionBindingRefresh::Unchanged);
}

TEST (SunStudySelectionBinding, RemovingIgnoredAndMovingBetweenSetsUpdatesBothRoles)
{
    SunStudySelectionBinding binding { "Context", "Ignored", 8, 10 };
    std::vector<std::string> context { "site" }, ignored { "excluded" };
    ASSERT_EQ (binding.Refresh (8, { "excluded" }, {}, context, ignored), SelectionBindingRefresh::Changed);
    const auto roles = ResolveElementRoles ({ "site", "excluded" }, {}, context, ignored);
    EXPECT_EQ (roles.SampleMask (), (std::vector<uint8_t> { 1, 0 }));
    EXPECT_TRUE (ignored.empty ());
    ASSERT_EQ (binding.Refresh (8, {}, { "site" }, context, ignored), SelectionBindingRefresh::Changed);
    EXPECT_EQ (ResolveElementRoles ({ "site", "excluded" }, {}, context, ignored).SampleMask (),
               (std::vector<uint8_t> { 0, 1 }));
}

TEST (SunStudySelectionBinding, EquivalentCanonicalPicksDoNotInvalidate)
{
    SunStudySelectionBinding binding { "Context", "Ignored", 8, 10 };
    std::vector<std::string> context { "a", "b" }, ignored { "c" };
    EXPECT_EQ (binding.Refresh (8, { "{B}", " A ", "b" }, { "{C}" }, context, ignored),
               SelectionBindingRefresh::Unchanged);
    EXPECT_EQ (context, (std::vector<std::string> { "a", "b" }));
}

TEST (SunStudySelectionBinding, SwitchingCommandsOrRescanningCannotConsumeAnotherCommandsSets)
{
    SunStudySelectionBinding binding { "Context", "Ignored", 8, 10 };
    std::vector<std::string> context { "site" }, ignored { "excluded" };
    EXPECT_EQ (binding.Refresh (9, {}, {}, context, ignored), SelectionBindingRefresh::Detached);
    EXPECT_EQ (context, (std::vector<std::string> { "site" }));
    EXPECT_EQ (ignored, (std::vector<std::string> { "excluded" }));
    EXPECT_TRUE (binding.contextSet.empty ());
    EXPECT_EQ (binding.generation, 0u);
    EXPECT_EQ (binding.Refresh (9, { "other" }, {}, context, ignored), SelectionBindingRefresh::Inactive);
}

TEST (SunStudySelectionBinding, ExplicitListsStayFixedAndPartialBindingRetainsOtherRole)
{
    SunStudySelectionBinding fixed;
    std::vector<std::string> context { "site" }, ignored { "excluded" };
    EXPECT_EQ (fixed.Refresh (8, {}, {}, context, ignored), SelectionBindingRefresh::Inactive);
    EXPECT_EQ (context, (std::vector<std::string> { "site" }));
    SunStudySelectionBinding binding { "Context", "", 8, 10 };
    EXPECT_EQ (binding.Refresh (8, {}, {}, context, ignored), SelectionBindingRefresh::Changed);
    EXPECT_TRUE (context.empty ());
    EXPECT_EQ (ignored, (std::vector<std::string> { "excluded" }));
}

TEST (SunStudySelectionBinding, RoleChangesSupersedeInFlightResultsWithoutClearingRenderableCache)
{
    SunStudySelectionBinding binding { "Context", "Ignored", 8, 10 };
    std::vector<std::string> context { "site" }, ignored;
    SunStudyFollower follower;
    SunStudyDependencySignature initial { 41, 7, 1 }, first { 41, 7, 2 }, latest { 41, 7, 3 };
    follower.Adopt ("accepted", initial, 1000);
    ASSERT_EQ (binding.Refresh (8, { "site", "another" }, {}, context, ignored), SelectionBindingRefresh::Changed);
    follower.Observe (first, 1100);
    const auto obsolete = follower.NoteStarted (first, 1400);
    ASSERT_EQ (binding.Refresh (8, {}, {}, context, ignored), SelectionBindingRefresh::Changed);
    follower.Observe (latest, 1450);
    EXPECT_FALSE (follower.CanPublishResult (obsolete, first));
    EXPECT_EQ (follower.DirtyReason (), SunStudyDirtyReason::Sampling);
    EXPECT_TRUE (follower.HasRenderableCache ());
    EXPECT_FALSE (follower.ShouldStart (1749));
    EXPECT_TRUE (follower.ShouldStart (1750));
    const auto generation = follower.NoteStarted (latest, 1750);
    EXPECT_TRUE (follower.CanPublishResult (generation, latest));
    EXPECT_TRUE (follower.NoteCompleted (generation, "restored", latest, 1800));
}
