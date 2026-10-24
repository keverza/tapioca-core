#include "hud_fixture.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include <gtest/gtest.h>
#include <cstring>

using namespace hudtest;
namespace shell = geomsrv::archviz::hudshell;

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
    const std::vector<shell::HostTab> tabs = { { "sun", "Sun study" }, { shell::kSettingsKey, "Settings" } };
    EXPECT_EQ (shell::ResolveNavigation (" workflow Sun study ", tabs), "sun");
    EXPECT_EQ (shell::ResolveNavigation ("show SETTINGS", tabs), shell::kSettingsKey);
    EXPECT_TRUE (shell::ResolveNavigation ("sun stu", tabs).empty ());
    EXPECT_TRUE (shell::ResolveNavigation ("run", tabs).empty ());
    EXPECT_TRUE (shell::ResolveNavigation ("bake", tabs).empty ());
    EXPECT_TRUE (shell::ResolveNavigation ("Sun study", { { "one", "Sun study" }, { "two", "Sun study" } }).empty ());
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

TEST (HudHierarchy, DockTitleOpensButOnlyTheSeparateControlCollapses)
{
    Watched hud;
    hud::OwnPages own;
    own.standalone = true;
    hud.engine.SetOwnPages (own);
    auto out = hud.Lay ({}, At (0, 0));
    const float x = 1200 + out.dock.offset[0] + out.dock.width / 2;
    const float top = 400 + out.dock.offset[1];
    const float titleY = top + out.dock.width * 2 + 8;
    hud.Click ({}, x, titleY);
    EXPECT_TRUE (hud::HudOpen (*hud.state));
    const float collapseY = top + out.dock.height - 1.5f * out.dock.width;
    hud.Click ({}, x, collapseY);
    EXPECT_FALSE (hud::HudOpen (*hud.state));
    hud.Click ({}, x, titleY);
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
    const float x = Left (out.host) + 30, y = Top (out.host) + out.host.height - 20;
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
