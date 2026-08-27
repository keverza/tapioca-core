// PlanOverlay/OverlayBinding.cpp — which window an overlay belongs to.
//
// Every failure this pins is silent in Archicad. An overlay that keeps tracking
// while a schedule is in front looks exactly like one that does not: the plan is
// not on screen, so nobody sees the geometry being drawn in the wrong place, and
// the only evidence is four ACAPI calls and a layered-window repaint per tick on
// the UI thread, for a window nobody is looking at. That is the "opening a
// schedule hoards overlay resources" report, and it is invisible until someone
// profiles.
//
// The opposite failure is just as quiet: an overlay that suspends when it should
// not simply never appears, and "the overlay is broken" points nowhere near a
// window-identity comparison.

#include "PlanOverlay/OverlayBinding.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::planoverlay;

namespace {

// The DevKit's own rule: "In the case of Floor Plan and 3D Model databases, the
// typeID field is enough to identify them."
OverlayWindowId FloorPlan ()
{
    OverlayWindowId id;
    id.typeId = 1; // APIWind_FloorPlanID
    return id;
}

OverlayWindowId Model3D ()
{
    OverlayWindowId id;
    id.typeId = 2; // APIWind_3DModelID
    return id;
}

OverlayWindowId Schedule ()
{
    OverlayWindowId id;
    id.typeId = 9; // a schedule/list window
    return id;
}

// Sections, details, worksheets and layouts are separated by databaseUnId, not
// by type: two sections are the same typeId and are not the same window.
OverlayWindowId Section (uint64_t guidLow)
{
    OverlayWindowId id;
    id.typeId = 4;
    id.databaseGuidHigh = 0xAAAA;
    id.databaseGuidLow = guidLow;
    return id;
}

} // namespace

TEST (OverlayBinding, TracksWhileItsOwnWindowIsInFront)
{
    const OverlayTrackDecision decision = DecideTrackTick (FloorPlan (), FloorPlan (), true);

    EXPECT_TRUE (decision.track);
    EXPECT_TRUE (decision.visible);
    EXPECT_FALSE (decision.visibilityChanged);
}

// The report this rule exists for: a schedule is a different window, so the
// overlay stops costing anything at all rather than polling and repainting for a
// plan nobody can see.
TEST (OverlayBinding, ASchedulePutsThePlanOverlayToSleepEntirely)
{
    const OverlayTrackDecision decision = DecideTrackTick (FloorPlan (), Schedule (), true);

    EXPECT_FALSE (decision.track);
    EXPECT_FALSE (decision.visible);
    EXPECT_TRUE (decision.visibilityChanged);
}

// ⚠️ THE 3D WINDOW IS NOT A SECOND CAMERA ON THE FLOOR PLAN. Different
// projection, different units, different meaning for a line's width; a plan
// overlay drawn over it is a wrong picture rather than a degraded one.
TEST (OverlayBinding, The3DWindowIsADifferentSurfaceAndNotTheBoundOne)
{
    const OverlayTrackDecision decision = DecideTrackTick (FloorPlan (), Model3D (), true);

    EXPECT_FALSE (decision.track);
    EXPECT_FALSE (decision.visible);
}

TEST (OverlayBinding, ComingBackToTheBoundWindowResumesAndSaysVisibilityChanged)
{
    const OverlayTrackDecision decision = DecideTrackTick (FloorPlan (), FloorPlan (), false);

    EXPECT_TRUE (decision.track);
    EXPECT_TRUE (decision.visible);
    EXPECT_TRUE (decision.visibilityChanged);
}

// Two sections share a typeId. Comparing type alone would let an overlay opened
// over one section keep drawing over another, which is the failure mode a
// per-type rule produces for whichever window type nobody thought about.
TEST (OverlayBinding, TwoWindowsOfTheSameTypeAreToldApartByTheirDatabase)
{
    EXPECT_TRUE (DecideTrackTick (Section (1), Section (1), true).track);
    EXPECT_FALSE (DecideTrackTick (Section (1), Section (2), true).track);
}

// Not knowing which window is in front is exactly the moment not to project into
// it -- and it is also what a project being closed looks like from inside a
// timer that is still running.
TEST (OverlayBinding, AnUnreadableCurrentWindowSuspendsRatherThanCarryingOn)
{
    const OverlayTrackDecision decision = DecideTrackTick (FloorPlan (), OverlayWindowId (), true);

    EXPECT_FALSE (decision.track);
    EXPECT_FALSE (decision.visible);
    EXPECT_TRUE (decision.visibilityChanged);
}

// The pre-binding behaviour, kept on purpose: a caller that never recorded a
// window gets the overlay it used to get, not a blank one it would read as
// broken.
TEST (OverlayBinding, AnUnboundOverlayKeepsTrackingWhateverIsInFront)
{
    const OverlayTrackDecision decision = DecideTrackTick (OverlayWindowId (), Schedule (), true);

    EXPECT_TRUE (decision.track);
    EXPECT_TRUE (decision.visible);
    EXPECT_FALSE (decision.visibilityChanged);
}

// A suspended overlay must not ask to be hidden on every tick: ShowWindow on a
// layered window is not free, and doing it 20 times a second while a schedule is
// open is the same kind of waste the suspension exists to stop.
TEST (OverlayBinding, StayingSuspendedReportsNoVisibilityChange)
{
    const OverlayTrackDecision decision = DecideTrackTick (FloorPlan (), Schedule (), false);

    EXPECT_FALSE (decision.track);
    EXPECT_FALSE (decision.visible);
    EXPECT_FALSE (decision.visibilityChanged);
}
