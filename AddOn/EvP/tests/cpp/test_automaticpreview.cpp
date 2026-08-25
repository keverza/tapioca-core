#include "Palette/AutomaticPreviewState.hpp"
#include "Python/ForcedParamMerge.hpp"

#include <gtest/gtest.h>

using evp::AutomaticPreviewState;

TEST (AutomaticPreviewState, DebouncesAndCoalescesSelectionChanges)
{
    AutomaticPreviewState state;
    const auto start = AutomaticPreviewState::Clock::time_point {};
    state.SelectionChanged (start);
    state.SelectionChanged (start + std::chrono::milliseconds (200));

    EXPECT_FALSE (state.ShouldLaunch (start + std::chrono::milliseconds (549), true, false));
    EXPECT_TRUE (state.ShouldLaunch (start + std::chrono::milliseconds (550), true, false));
    EXPECT_FALSE (state.ShouldLaunch (start + std::chrono::milliseconds (550), false, false));
    EXPECT_FALSE (state.ShouldLaunch (start + std::chrono::milliseconds (550), true, true));
}

TEST (AutomaticPreviewState, SelectionChangedInFlightSchedulesOneRerun)
{
    AutomaticPreviewState state;
    const auto start = AutomaticPreviewState::Clock::time_point {};
    state.SelectionChanged (start);
    state.Started (7);
    state.SelectionChanged (start + std::chrono::milliseconds (100));

    EXPECT_FALSE (state.ShouldLaunch (start + std::chrono::seconds (1), true, false));
    EXPECT_FALSE (state.Finished (6));
    EXPECT_TRUE (state.Finished (7));
    EXPECT_TRUE (state.ShouldLaunch (start + std::chrono::seconds (1), true, false));
    state.Started (8);
    EXPECT_FALSE (state.ShouldLaunch (start + std::chrono::seconds (2), true, false));
}

TEST (AutomaticPreviewState, CancelDropsPendingAndInFlightOwnership)
{
    AutomaticPreviewState state;
    const auto start = AutomaticPreviewState::Clock::time_point {};
    state.SelectionChanged (start);
    state.Started (3);
    state.SelectionChanged (start);
    state.Cancel ();

    EXPECT_FALSE (state.IsInFlight ());
    EXPECT_FALSE (state.Finished (3));
    EXPECT_FALSE (state.ShouldLaunch (start + std::chrono::seconds (1), true, false));
}

TEST (ForcedParamMerge, ForcedMembersAreLastAndEmptyOverridesAreNoOp)
{
    EXPECT_EQ (evp::MergeForcedParams (R"({"apply_changes":true,"start":4})", R"({"apply_changes":false})"),
               R"({"apply_changes":true,"start":4,"apply_changes":false})");
    EXPECT_EQ (evp::MergeForcedParams (R"({"start":4})", "{}"), R"({"start":4})");
}
