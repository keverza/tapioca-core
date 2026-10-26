#include "hud_fixture.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>

using namespace hudtest;
namespace shell = geomsrv::archviz::hudshell;

TEST (HudHierarchy, DefaultDockStartsAtTopLeftAndPanelMeetsItsLowerEdge)
{
    shell::DockPlacement dock;
    EXPECT_EQ (dock.edge, shell::DockEdge::Top);
    EXPECT_TRUE (dock.horizontal);
    EXPECT_FLOAT_EQ (dock.along, 0);
    const ImVec2 pos = shell::DockPosition (dock, { 1200, 800 }, { 200, 28 }, 1);
    EXPECT_FLOAT_EQ (pos.x, 0);
    EXPECT_FLOAT_EQ (pos.y, 0);
    const auto panel =
        shell::PanelAtDock ({ 1200, 800 }, pos, { 200, 28 }, dock.edge, dock.horizontal, { 320, 350 }, shell::kDockGap);
    EXPECT_FLOAT_EQ (panel.position.x, 0);
    EXPECT_FLOAT_EQ (panel.position.y, 28);
    EXPECT_EQ (shell::kDockGap, 0);
}

TEST (HudHierarchy, DockIsPulledTowardEdgesAndCornersBeforeRelease)
{
    const ImVec2 view (1200, 800), size (200, 28);
    const ImVec2 untouched = shell::MagneticDockPosition ({ 200, 180 }, view, size, 1);
    EXPECT_FLOAT_EQ (untouched.x, 200);
    EXPECT_FLOAT_EQ (untouched.y, 180);
    const ImVec2 edge = shell::MagneticDockPosition ({ 200, 18 }, view, size, 1);
    EXPECT_LT (edge.y, 18);
    EXPECT_GT (edge.y, 0);
    const ImVec2 corner = shell::MagneticDockPosition ({ 18, 18 }, view, size, 1);
    EXPECT_LT (corner.x, 18);
    EXPECT_LT (corner.y, 18);
    shell::DockPlacement dock;
    shell::SnapDock (dock, { 200, 18 }, view, size, 1);
    EXPECT_EQ (dock.edge, shell::DockEdge::Top);
}

TEST (HudHierarchy, DockSnapsToAllEdgesAndCornersAndStaysAttachedAfterResize)
{
    for (float scale : { 1.0f, 2.0f }) {
        const ImVec2 view (1200 * scale, 800 * scale), size (24 * scale, 180 * scale);
        for (const auto edge :
             { shell::DockEdge::Top, shell::DockEdge::Bottom, shell::DockEdge::Left, shell::DockEdge::Right }) {
            for (float along : { 0.0f, 0.5f, 1.0f }) {
                shell::DockPlacement p;
                p.edge = edge;
                p.horizontal = edge == shell::DockEdge::Top || edge == shell::DockEdge::Bottom;
                p.along = along;
                const ImVec2 dimensions = p.horizontal ? ImVec2 (size.y, size.x) : size;
                auto position = shell::DockPosition (p, view, dimensions, scale);
                shell::SnapDock (p, position, view, dimensions, scale);
                EXPECT_EQ (p.edge, edge);
                EXPECT_FLOAT_EQ (p.along, along);
                const ImVec2 resized (900 * scale, 600 * scale);
                position = shell::DockPosition (p, resized, dimensions, scale);
                const ImVec2 end (position.x + dimensions.x, position.y + dimensions.y);
                auto panel = shell::PanelAtDock (resized, position, end, p.edge, p.horizontal,
                                                 ImVec2 (400 * scale, 700 * scale), 6 * scale);
                EXPECT_GE (panel.position.x, 0);
                EXPECT_GE (panel.position.y, 0);
                EXPECT_LE (panel.position.x + panel.size.x, resized.x);
                EXPECT_LE (panel.position.y + panel.size.y, resized.y);
                if (edge == shell::DockEdge::Top)
                    EXPECT_GE (panel.position.y, end.y);
                if (edge == shell::DockEdge::Bottom)
                    EXPECT_LE (panel.position.y + panel.size.y, position.y);
                if (edge == shell::DockEdge::Left)
                    EXPECT_GE (panel.position.x, end.x);
                if (edge == shell::DockEdge::Right)
                    EXPECT_LE (panel.position.x + panel.size.x, position.x);
            }
        }
    }
}

TEST (HudHierarchy, FloatingDockKeepsItsPositionAndOpensTowardAvailableSpace)
{
    shell::DockPlacement p;
    shell::SnapDock (p, { 300, 200 }, { 1200, 800 }, { 24, 180 }, 1);
    EXPECT_EQ (p.edge, shell::DockEdge::Floating);
    const auto pos = shell::DockPosition (p, { 1200, 800 }, { 24, 180 }, 1);
    EXPECT_FLOAT_EQ (pos.x, 300);
    EXPECT_FLOAT_EQ (pos.y, 200);
    const auto panel = shell::PanelAtDock ({ 1200, 800 }, pos, { 324, 380 }, p.edge, false, { 320, 500 }, 6);
    EXPECT_GE (panel.position.x, 324);
    EXPECT_LE (panel.position.y + panel.size.y, 800);
}

TEST (HudHierarchy, NavigationRequiresAnExactUniquePageAndNeverResolvesModelCommands)
{
    std::vector<shell::HostTab> tabs = { { "sun", "Sun study" }, { shell::kSettingsKey, "Settings" } };
    tabs[0].token = "sunstudy";
    EXPECT_EQ (shell::ResolveNavigation (" workflow Sun study ", tabs), "sun");
    EXPECT_EQ (shell::ResolveNavigation ("workflow SUNSTUDY", tabs), "sun");
    EXPECT_EQ (shell::ResolveNavigation ("show SETTINGS", tabs), shell::kSettingsKey);
    EXPECT_TRUE (shell::ResolveNavigation ("sun stu", tabs).empty ());
    EXPECT_TRUE (shell::ResolveNavigation ("run", tabs).empty ());
    EXPECT_TRUE (shell::ResolveNavigation ("bake", tabs).empty ());
    EXPECT_TRUE (shell::ResolveNavigation ("Sun study", { { "one", "Sun study" }, { "two", "Sun study" } }).empty ());
    tabs.push_back ({ "other", "Another study" });
    tabs.back ().token = "sunstudy";
    EXPECT_TRUE (shell::ResolveNavigation ("workflow sunstudy", tabs).empty ());
}

TEST (HudHierarchy, InvalidNavigationShowsAReadableErrorCard)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    Navigate (hud, {}, "not a workflow");
    const auto out = hud.Lay ({}, At (0, 0));
    float box[4] = {};
    EXPECT_TRUE (Box (out.host, 0xFFF1EFFFu, box));
    EXPECT_TRUE (Box (out.host, 0x34201FFFu, box));
}

TEST (HudHierarchy, NativeVirtualEditorIsAvailableWithoutAPublishedPanel)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    Navigate (hud, {}, "virtual editor");
    EXPECT_EQ (hud.engine.Selected (), "virtual-editor#0");
    EXPECT_FALSE (hud::VirtualEditorActive (*hud.state));
    ASSERT_TRUE (hud::OpenVirtualEditorWorkflow (*hud.state));
    EXPECT_TRUE (hud::VirtualEditorActive (*hud.state));
    const auto* session = hud::VirtualEditor (*hud.state);
    ASSERT_TRUE (session->edits.Doc ().landscape.has_value ());
    std::string error;
    EXPECT_TRUE (geomsrv::edit::Validate (session->edits.Doc (), error)) << error;
    ASSERT_TRUE (hud::OpenVirtualEditorWorkflow (*hud.state));
    EXPECT_EQ (hud::VirtualEditor (*hud.state), session);
    Navigate (hud, {}, "Settings");
    EXPECT_TRUE (hud::VirtualEditorActive (*hud.state));
    hud::StopVirtualEditor (*hud.state, "test complete");
    Navigate (hud, {}, "virtual editor");
    EXPECT_EQ (hud.engine.Selected (), "virtual-editor#0");
    layers::Clear ("virtual-editor");
}

TEST (HudHierarchy, ViewerEditorKeepsAnalysisInNativeNavigation)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    own.viewerSurface = true;
    hud.engine.SetOwnPages (own);
    Navigate (hud, {}, "Virtual editor");
    EXPECT_EQ (hud.engine.Selected (), "virtual-editor#0");
    Navigate (hud, {}, "Analysis");
    EXPECT_EQ (hud.engine.Selected (), "@viewer.analysis");
}

TEST (HudHierarchy, WorkflowFlyoutUsesTheSideWithRoom)
{
    const ImVec2 view (1200, 800), wanted (180, 160);
    auto flyout = shell::WorkflowFlyoutAtDock (view, { 0, 0 }, { 200, 28 }, wanted);
    EXPECT_EQ (flyout.side, shell::DockEdge::Right);
    EXPECT_FLOAT_EQ (flyout.position.x, 200);
    flyout = shell::WorkflowFlyoutAtDock (view, { 1000, 0 }, { 1200, 28 }, wanted);
    EXPECT_EQ (flyout.side, shell::DockEdge::Left);
    EXPECT_FLOAT_EQ (flyout.position.x + flyout.size.x, 1000);
    flyout = shell::WorkflowFlyoutAtDock (view, { 500, 0 }, { 700, 28 }, wanted);
    EXPECT_EQ (flyout.side, shell::DockEdge::Right);
    EXPECT_FLOAT_EQ (flyout.position.x, 700);
    flyout = shell::WorkflowFlyoutAtDock (view, { 500, 772 }, { 700, 800 }, wanted);
    EXPECT_EQ (flyout.side, shell::DockEdge::Right);
    EXPECT_FLOAT_EQ (flyout.position.y + flyout.size.y, 800);
    flyout = shell::WorkflowFlyoutAtDock (view, { 0, 200 }, { 28, 400 }, { 22, 160 });
    EXPECT_EQ (flyout.side, shell::DockEdge::Bottom);
    EXPECT_FLOAT_EQ (flyout.position.x, 3);
    EXPECT_FLOAT_EQ (flyout.position.y, 400);
    flyout = shell::WorkflowFlyoutAtDock (view, { 300, 0 }, { 500, 28 }, { 180, 22 });
    EXPECT_FLOAT_EQ (flyout.position.y, 3);
}

TEST (HudHierarchy, DockTitleOpensWorkflowTabsWithoutSwitchingOrTakingTextFocus)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    layers::Panel panel;
    panel.title = "My workflow";
    const auto open = hud.Lay ({ &panel }, At (0, 0));
    const float x = Left (open.dock) + open.dock.height * 2 + 8;
    const float y = Top (open.dock) + open.dock.height * 0.5f;
    const std::string selected = hud.engine.Selected ();
    const auto menu = hud.Click ({ &panel }, x, y);
    EXPECT_TRUE (menu.popup);
    EXPECT_EQ (hud.engine.Selected (), selected);
    EXPECT_FALSE (hud.state->criteriaTextActive);
    EXPECT_FALSE (hud.Click ({ &panel }, x, y).popup) << "a second dock-title click closes the workflow list";
    EXPECT_EQ (hud.engine.Selected (), selected);
    EXPECT_TRUE (hud.Click ({ &panel }, x, y).popup) << "a third click opens it again";
}

TEST (HudHierarchy, DockLabelWeightPassStaysOnWholePixelsWhenTurned)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    for (const auto edge : { shell::DockEdge::Top, shell::DockEdge::Right }) {
        hud.state->host.dock.edge = edge;
        hud.state->host.dock.along = 0.5f;
        const auto out = hud.Lay ({}, At (0, 0));
        size_t weightedGlyphVertices = 0;
        for (const auto& vertex : out.dock.vertices) {
            if ((vertex.rgba & 0xFFu) != 82u)
                continue;
            ++weightedGlyphVertices;
            EXPECT_NEAR (vertex.x, std::round (vertex.x), 1e-3f);
            EXPECT_NEAR (vertex.y, std::round (vertex.y), 1e-3f);
        }
        EXPECT_GT (weightedGlyphVertices, 8u);
    }
}

TEST (HudHierarchy, AnUnavailableWorkflowIsNotReplacedAndItsDraftSurvives)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    layers::Panel panel;
    panel.title = "My workflow";
    hud.Lay ({ &panel }, At (0, 0));
    hud::SelectKey (*hud.state, "hud#0");
    hud.Lay ({ &panel }, At (0, 0));
    hud.Lay ({}, At (0, 0));
    EXPECT_EQ (hud.engine.Selected (), "hud#0");
    EXPECT_FALSE (hud.state->criteriaTextActive);
}

TEST (HudHierarchy, DockOrientationAndPanelBoundsFollowEveryAnchor)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    for (auto edge : { shell::DockEdge::Top, shell::DockEdge::Bottom, shell::DockEdge::Left, shell::DockEdge::Right }) {
        for (float along : { 0.0f, 0.5f, 1.0f }) {
            hud.state->host.dock.edge = edge;
            hud.state->host.dock.along = along;
            hud.Lay ({}, At (0, 0));
            const auto out = hud.Lay ({}, At (0, 0));
            const bool horizontal = edge == shell::DockEdge::Top || edge == shell::DockEdge::Bottom;
            EXPECT_EQ (out.dock.width > out.dock.height, horizontal);
            const float x = out.host.fraction[0] * 1200 + out.host.offset[0];
            const float y = out.host.fraction[1] * 800 + out.host.offset[1];
            EXPECT_GE (x, -1);
            EXPECT_GE (y, -1);
            EXPECT_LE (x + out.host.width, 1201);
            EXPECT_LE (y + out.host.height, 801);
            EXPECT_FALSE (hud.state->criteriaTextActive);
        }
    }
}

TEST (HudHierarchy, DockArrowCollapsesAndExpandsThePanel)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    auto out = hud.Lay ({}, At (0, 0));
    const float left = Left (out.dock), top = Top (out.dock);
    const float y = top + out.dock.height * 0.5f;
    const float titleX = left + out.dock.height * 2 + 8;
    const float collapseX = left + out.dock.width - 1.5f * out.dock.height;
    hud.Click ({}, collapseX, y);
    EXPECT_FALSE (hud::HudOpen (*hud.state));
    const auto choices = hud.Click ({}, titleX, y);
    EXPECT_TRUE (choices.popup);
    EXPECT_FALSE (hud::HudOpen (*hud.state));
    hud.Click ({}, collapseX, y);
    EXPECT_TRUE (hud::HudOpen (*hud.state));
    EXPECT_FALSE (hud.state->criteriaTextActive);
}

TEST (HudHierarchy, TypedNavigationUsesExplicitFocusAndPanelArrivalKeepsTheCaret)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    Navigate (hud, {}, "show Settings");
    ASSERT_EQ (hud.engine.Selected (), shell::kSettingsKey);
    auto out = hud.Lay ({}, At (0, 0));
    EXPECT_FALSE (hud.state->criteriaTextActive);
    const float x = Left (out.host) + 30, y = CommandY (out);
    hud.Click ({}, x, y);
    ASSERT_TRUE (hud.state->criteriaTextActive);
    auto input = At (x, y);
    input.characters = u"show Se";
    hud.Lay ({}, input);
    layers::Panel panel;
    panel.title = "Background result";
    input.characters.clear ();
    hud.Lay ({ &panel }, input);
    EXPECT_EQ (hud.engine.Selected (), shell::kSettingsKey);
    ASSERT_TRUE (hud.state->criteriaTextActive);
    input.characters = u"lection";
    hud.Lay ({ &panel }, input);
    input.characters.clear ();
    input.keys = { { 0x0D, true } };
    hud.Lay ({ &panel }, input);
    input.keys = { { 0x0D, false } };
    hud.Lay ({ &panel }, input);
    EXPECT_EQ (hud.engine.Selected (), shell::kSelectionKey);
    hud::SetHudOpen (*hud.state, false);
    hud.Lay ({ &panel }, At (0, 0));
    EXPECT_FALSE (hud.state->criteriaTextActive);
    hud::SetHudOpen (*hud.state, true);
    hud.Lay ({ &panel }, At (0, 0));
    EXPECT_FALSE (hud.state->criteriaTextActive) << "reopening never requests focus";
}
