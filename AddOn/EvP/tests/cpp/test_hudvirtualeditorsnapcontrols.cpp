#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <gtest/gtest.h>

namespace eh = geomsrv::archviz::edithud;
namespace ed = geomsrv::edit;

TEST (HudVirtualEditorSnapControls, EverySupportedModeHasItsOwnOptionalToggle)
{
    uint32_t covered = 0;
    for (const auto& option : eh::SnapOptions ()) {
        EXPECT_NE (option.mode, 0u);
        EXPECT_EQ (option.mode & (option.mode - 1), 0u);
        EXPECT_EQ (covered & option.mode, 0u);
        EXPECT_NE (option.label, nullptr);
        EXPECT_NE (option.label[0], '\0');
        covered |= option.mode;
    }
    EXPECT_EQ (covered, ed::kSnapAll);
}

TEST (HudVirtualEditorSnapControls, SuspendingSnapsRetainsTheModeChoicesAndHistory)
{
    eh::Session s;
    s.snapModes = ed::kSnapEndpoint | ed::kSnapMidpoint;
    s.menu.target = eh::MenuTarget::View;
    bool offered = false;
    for (const auto& item : eh::MenuItems (s))
        offered = offered || item.id == "snap.enabled";
    ASSERT_TRUE (offered);
    eh::RunMenuItem (s, "snap.enabled");
    EXPECT_FALSE (s.snapEnabled);
    EXPECT_EQ (s.snapModes, ed::kSnapEndpoint | ed::kSnapMidpoint);
    ed::SnapQuery query;
    eh::detail::PrepareSnap (s, query);
    EXPECT_EQ (query.modes, 0u);
    eh::RunMenuItem (s, "snap.enabled");
    EXPECT_TRUE (s.snapEnabled);
    EXPECT_EQ (s.snapModes, ed::kSnapEndpoint | ed::kSnapMidpoint);
    eh::detail::PrepareSnap (s, query);
    EXPECT_EQ (query.modes, s.snapModes);
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
}

TEST (HudVirtualEditorSnapControls, GridAndTerrainElevationStartAsExplicitOptIns)
{
    eh::Session s;
    EXPECT_EQ (s.snapModes & (ed::kSnapTerrain | ed::kSnapGrid), 0u);
    EXPECT_GT (s.snapGrid, 0);
}

TEST (HudVirtualEditorSnapControls, AThreeDProximitySettingAlwaysKeepsAPositiveBound)
{
    eh::Session s;
    geomsrv::archviz::overlayhud::Input input;
    input.planar = false;
    eh::detail::Map view { input };
    ed::SnapQuery query;
    s.snapProximity = 2.5;
    eh::detail::ScreenPointer (s, view, {}, 1, query);
    ASSERT_TRUE (query.screen);
    EXPECT_DOUBLE_EQ (query.screen->proximity, 2.5);
    s.snapProximity = 0;
    eh::detail::ScreenPointer (s, view, {}, 1, query);
    EXPECT_GT (query.screen->proximity, 0);
}
