#include "PlanOverlay/OverlayOwnership.hpp"

#include <gtest/gtest.h>

namespace {

namespace overlay = geomsrv::planoverlay;

TEST (OverlayOwnership, WatchDestroysOnlyItsMatchingWindow)
{
    const overlay::SessionOwnership session { overlay::Owner::Watch, true, true };

    EXPECT_EQ (overlay::DetermineReleaseAction (session, overlay::Owner::Watch, true),
               overlay::ReleaseAction::DestroyWindow);
    EXPECT_EQ (overlay::DetermineReleaseAction (session, overlay::Owner::NativeCommand, true),
               overlay::ReleaseAction::None);
    EXPECT_EQ (overlay::DetermineReleaseAction (session, overlay::Owner::Watch, false), overlay::ReleaseAction::None);
}

TEST (OverlayOwnership, BorrowerRestoresOnlyTrackingItStarted)
{
    overlay::SessionOwnership session { overlay::Owner::Watch, false, true };
    EXPECT_EQ (overlay::DetermineReleaseAction (session, overlay::Owner::NativeCommand, true),
               overlay::ReleaseAction::StopTracking);

    session.startedTracking = false;
    EXPECT_EQ (overlay::DetermineReleaseAction (session, overlay::Owner::NativeCommand, true),
               overlay::ReleaseAction::None);
}

} // namespace
