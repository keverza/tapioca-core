// ArchViz/OverlayHud, stage 2: what the user presses on the HUD -- the tint and the hand
// over it, the floating host and its dock, its Settings, and the controls whose values the
// add-on holds and reports to Python. Laid out by the vendored Dear ImGui, as the add-on
// does, and pressed as the input layer hands the presses over.

#include "hud_fixture.hpp"

#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace hudtest;

namespace input = geomsrv::archviz::overlayinput;
namespace scene = geomsrv::archviz::overlayscene;
namespace shell = geomsrv::archviz::hudshell;

namespace {

void RightDock (const std::shared_ptr<hud::State>& state)
{
    state->host.dock.edge = shell::DockEdge::Right;
    state->host.dock.along = 0.5f;
    state->host.dock.horizontal = false;
}

uint32_t WithAlpha (uint32_t rgba, float factor)
{
    const uint32_t a = uint32_t (std::lround (float (rgba & 0xFFu) * factor));
    return (rgba & 0xFFFFFF00u) | (std::min) (a, 255u);
}

} // namespace

// ⚠️ THE USER, 2026-09-29: a hand over what the HUD can press, and a tint on it. A
// section's row is pressable: pointed at, it is tinted and the layout asks for the hand; over
// the panel's still parts, and off it, an arrow. The tint is the card's grey ink, not its accent
// (the user, 2026-10-21), and stronger on this dark card (HudShell.cpp `PushLook`).
TEST (OverlayHudControls, APressableRowIsTintedAndShowsTheHand)
{
    Fresh hud;
    layers::Panel panel;
    panel.items.push_back (Item (layers::ItemKind::Section, "Spacing metrics"));
    layers::PanelItem gap = Item (layers::ItemKind::Spacing);
    gap.heightPixels = 40.0f;
    panel.items.push_back (gap);
    const uint32_t tint = WithAlpha (panel.textRgba, 0.16f * 1.6f);
    // The panel at (16, 16), its padding in: the section's row, then the gap under it.
    const float left = 16.0f + panel.paddingPixels, top = 16.0f + panel.paddingPixels;
    const hud::Layout away = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (away.hand);
    EXPECT_FALSE (Drawn (away, tint));
    const hud::Layout row = hud.Lay ({ &panel }, At (left + 30.0f, top + 6.0f));
    EXPECT_TRUE (row.hand);
    EXPECT_TRUE (Drawn (row, tint)) << "the row tinted with the card's ink";
    const hud::Layout still = hud.Lay ({ &panel }, At (left + 30.0f, top + 45.0f));
    EXPECT_FALSE (still.hand) << "the gap under it is the HUD's, but nothing to press";
    EXPECT_FALSE (Drawn (still, tint));
    // The light card's accent is its own.
    layers::Panel light;
    layers::ApplyTheme (light, layers::PanelTheme::Light);
    EXPECT_EQ (light.accentRgba, 0x2F6FEBFFu);
}

// ---- the floating panel and its dock ---------------------------------------------------------

namespace {

// A titled panel -- a tab of the host -- whose page is a key of one colour, so which page
// the host shows can be read from its triangles.
layers::Panel Titled (const char* title = "Area metrics", uint32_t colour = 0x2A7F86FFu)
{
    layers::Panel panel;
    panel.title = title;
    layers::PanelItem swatch = Item (layers::ItemKind::Swatch, title);
    swatch.rgba = colour;
    panel.items.push_back (swatch);
    panel.items.push_back (Item (layers::ItemKind::Row, "BCR", "20.0 %"));
    return panel;
}

// The host's tab row, on a panel at the view's top-left (16, 16): a padding in, as tall as a
// frame (the font's 14 and 3 above and below).
constexpr float kRowY = 16.0f + 10.0f + 10.0f;

// Where the pointer shows the hand along the host's tab row, left to right: its tabs,
// Settings, then the close button at its end.
std::vector<std::pair<float, float>> Presses (Fresh& hud, const std::vector<const layers::Panel*>& panels,
                                              const hud::Built& host)
{
    std::vector<std::pair<float, float>> runs;
    const float x0 = 16.0f, x1 = 16.0f + host.width;
    bool in = false;
    for (float x = x0; x < x1; x += 2.0f) {
        const bool hand = hud.Lay (panels, At (x, kRowY)).hand;
        if (hand && !in)
            runs.push_back ({ x, x });
        if (hand)
            runs.back ().second = x;
        in = hand;
    }
    return runs;
}

float Middle (const std::pair<float, float>& run)
{
    return (run.first + run.second) * 0.5f;
}

// Where the pointer shows the hand down the host's page at `x`, top to bottom, under the tab
// row: its controls.
std::vector<std::pair<float, float>> Controls (Fresh& hud, const std::vector<const layers::Panel*>& panels,
                                               const hud::Built& host, float x)
{
    std::vector<std::pair<float, float>> runs;
    bool in = false;
    for (float y = Top (host) + 55; y < Top (host) + host.height - 36; y += 2.0f) {
        const bool hand = hud.Lay (panels, At (x, y)).hand;
        if (hand && !in)
            runs.push_back ({ y, y });
        if (hand)
            runs.back ().second = y;
        in = hand;
    }
    return runs;
}

// The rows of one colour's triangles, top to bottom: a list's lines of text -- those right
// of `left` alone, when a menu's submenu stands beside it.
std::vector<std::pair<float, float>> Rows (const hud::Built& built, uint32_t rgba, float left = -FLT_MAX)
{
    std::vector<std::pair<float, float>> spans;
    for (size_t t = 0; t + 2 < built.vertices.size (); t += 3) {
        if (built.vertices[t].rgba != rgba)
            continue;
        float lo = FLT_MAX, hi = -FLT_MAX, x = FLT_MAX;
        for (size_t k = t; k < t + 3; ++k) {
            lo = (std::min) (lo, built.vertices[k].y);
            hi = (std::max) (hi, built.vertices[k].y);
            x = (std::min) (x, built.vertices[k].x);
        }
        if (x >= left)
            spans.push_back ({ lo, hi });
    }
    std::sort (spans.begin (), spans.end ());
    std::vector<std::pair<float, float>> rows;
    for (const auto& span : spans)
        if (!rows.empty () && span.first <= rows.back ().second)
            rows.back ().second = (std::max) (rows.back ().second, span.second);
        else
            rows.push_back (span);
    return rows;
}

// The host on its Settings tab: the press before the close button along the tab row.
void OpenSettings (Watched& hud, const std::vector<const layers::Panel*>& panels)
{
    Navigate (hud, panels, "show Settings");
}

} // namespace

// ⚠️ THE USER, 2026-09-30: the STUDY panel's design as the main one, its tabs switching
// between the studies. Every titled panel is a tab of one host, drawn as the host's; its
// own entry holds nothing. The host shows the first; a press on another tab shows that
// one's page instead and says so. Set again, it stays on the user's tab.
TEST (OverlayHudControls, TitledPanelsAreTabsOfOneHost)
{
    Watched hud;
    const layers::Panel sun = Titled ("Sun study", 0x11AA22FFu);
    const layers::Panel area = Titled ("Area metrics", 0x3355CCFFu);
    const hud::Layout first = hud.Lay ({ &sun, &area }, At (600.0f, 600.0f));
    EXPECT_TRUE (first.panels[0].vertices.empty ());
    EXPECT_TRUE (first.panels[1].vertices.empty ());
    ASSERT_GT (first.host.height, 0.0f);
    float box[4] = {};
    EXPECT_TRUE (Box (first.host, 0x11AA22FFu, box)) << "the first tab's page";
    EXPECT_FALSE (Box (first.host, 0x3355CCFFu, box));
    EXPECT_EQ (first.hostKey, "hud#0");
    Navigate (hud, { &sun, &area }, "workflow Area metrics");
    const hud::Layout second = hud.Lay ({ &sun, &area }, At (600.0f, 600.0f));
    EXPECT_TRUE (Box (second.host, 0x3355CCFFu, box));
    EXPECT_FALSE (Box (second.host, 0x11AA22FFu, box));
    EXPECT_EQ (hud.engine.Selected (), "hud#1");
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "panel");
    EXPECT_EQ (hud.heard[0].key, "hud#1");
    EXPECT_EQ (hud.heard[0].text, "Area metrics");
    const layers::Panel sunAgain = Titled ("Sun study", 0x11AA22FFu), areaAgain = Titled ("Area metrics", 0x3355CCFFu);
    EXPECT_TRUE (Box (hud.Lay ({ &sunAgain, &areaAgain }, At (600.0f, 600.0f)).host, 0x3355CCFFu, box));
}

TEST (OverlayHudControls, ActiveWorkflowTabReturnsFromSettings)
{
    Watched hud;
    RightDock (hud.state);
    const layers::Panel panel = Titled ("Area metrics");
    OpenSettings (hud, { &panel });
    const hud::Layout settings = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    ASSERT_EQ (hud.engine.Selected (), "tapioca.settings");
    const float x = Left (settings.host) + 40.0f;
    const float y = Top (settings.host) + 16.0f;
    EXPECT_TRUE (hud.Lay ({ &panel }, At (x, y)).hand);
    hud.Click ({ &panel }, x, y);
    EXPECT_EQ (hud.engine.Selected (), "hud#0");
    EXPECT_FALSE (hud.state->criteriaTextActive);
}

// The side dock is vertical and attached to the panel. Its separate arrow toggles
// the panel; the turned title opens workflow choices without changing panel state.
TEST (OverlayHudControls, OneTurnedTabHasASeparatePanelArrow)
{
    Watched hud;
    RightDock (hud.state);
    const layers::Panel panel = Titled ();
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    ASSERT_GT (open.dock.height, 0.0f);
    EXPECT_GT (open.dock.height, 2.0f * open.dock.width) << "one slim tab, its title along it";
    EXPECT_FLOAT_EQ (open.dock.fraction[0], 1.0f);
    EXPECT_FLOAT_EQ (open.dock.offset[0], -open.dock.width) << "flush with the view's right edge";
    float box[4] = {};
    EXPECT_TRUE (Box (open.dock, panel.accentRgba, box)) << "filled while the host is open";
    // Its title turned: the white letters run down, not across.
    float letters[4] = {};
    ASSERT_TRUE (Box (open.dock, 0xFFFFFFFFu, letters));
    EXPECT_GT (letters[3] - letters[1], 3.0f * (letters[2] - letters[0]));
    const float x = 1200.0f + open.dock.offset[0] + open.dock.width * 0.5f;
    const float y = 400.0f + open.dock.offset[1] + open.dock.height * 0.5f;
    EXPECT_TRUE (hud.Lay ({ &panel }, At (x, y)).hand);

    const float collapseY = 400.0f + open.dock.offset[1] + open.dock.height - open.dock.width * 0.5f;
    hud.Click ({ &panel }, x, collapseY);
    const hud::Layout closed = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (hud.engine.Open ());
    EXPECT_TRUE (closed.host.vertices.empty ());
    EXPECT_FALSE (Box (closed.dock, panel.accentRgba, box)) << "in the card's colours while closed";
    hud.Click ({ &panel }, x, collapseY);
    EXPECT_TRUE (hud.engine.Open ());
    const hud::Layout choices = hud.Click ({ &panel }, x, y);
    EXPECT_TRUE (choices.popup);
    EXPECT_TRUE (hud.engine.Open ()) << "the workflow picker does not collapse the panel";
    ASSERT_EQ (hud.heard.size (), 2u);
    EXPECT_EQ (hud.heard[0].kind, "hud");
    EXPECT_EQ (hud.heard[0].text, "closed");
    EXPECT_EQ (hud.heard[1].text, "open");
}

// A layer's first titled panel asked to start collapsed starts the host closed; a panel
// without a title is no tab and stands alone at its anchor.
TEST (OverlayHudControls, TheHostStartsClosedWhenAskedAndAnUntitledPanelStandsAlone)
{
    Fresh hud;
    layers::Panel panel = Titled ();
    panel.collapsed = true;
    layers::Panel plain;
    plain.anchor = layers::PanelAnchor::BottomLeft;
    plain.items.push_back (Item (layers::ItemKind::Text, "Always here"));
    const hud::Layout out = hud.Lay ({ &panel, &plain }, At (600.0f, 600.0f));
    EXPECT_FALSE (hud.engine.Open ());
    EXPECT_TRUE (out.host.vertices.empty ());
    EXPECT_GT (out.dock.height, 0.0f);
    EXPECT_GT (out.panels[1].height, 0.0f);
    Fresh other;
    const hud::Layout alone = other.Lay ({ &plain }, At (600.0f, 600.0f));
    EXPECT_GT (alone.panels[0].height, 0.0f);
    EXPECT_FLOAT_EQ (alone.dock.width, 0.0f) << "no titled panel, no dock";
    EXPECT_FLOAT_EQ (alone.host.width, 0.0f);
}

// ⚠️ THE USER, 2026-09-30: floating, for the user to place wherever they want in the view.
// Dragged by its background, the host goes with the pointer; left nearer the view's
// bottom-right corner, it keeps its distance from that corner's edges when the view grows.
TEST (OverlayHudControls, TheDockMovesTheAttachedHost)
{
    Watched hud;
    RightDock (hud.state);
    const layers::Panel panel = Titled ();
    const auto before = hud.Lay ({ &panel }, At (0, 0));
    const float x = Left (before.dock) + before.dock.width / 2;
    const float y = Top (before.dock) + before.dock.width * 2 + 12;
    hud.Lay ({ &panel }, At (x, y));
    hud.Lay ({ &panel }, At (x, y, { { 0, true } }));
    hud.Lay ({ &panel }, At (x - 300, y - 120));
    hud.Lay ({ &panel }, At (x - 300, y - 120, { { 0, false } }));
    const auto after = hud.Lay ({ &panel }, At (0, 0));
    EXPECT_NEAR (Left (after.dock), Left (before.dock) - 300, 1);
    EXPECT_NEAR (Top (after.dock), Top (before.dock) - 120, 1);
    EXPECT_NE (Left (after.host), Left (before.host));
    EXPECT_TRUE (hud.engine.Open ());
    EXPECT_GE (Left (after.host), 0);
    EXPECT_LE (Left (after.host) + after.host.width, 1200);
    EXPECT_TRUE (hud.heard.empty ()) << "moving never navigates or executes";
}

// A panel without a title on the view's right column moves in beside the dock.
TEST (OverlayHudControls, TheRightColumnMovesInBesideTheDock)
{
    Fresh hud;
    auto state = hud::NewState ();
    RightDock (state);
    hud.engine.UseState (state);
    const layers::Panel titled = Titled ();
    layers::Panel right;
    right.anchor = layers::PanelAnchor::TopRight;
    right.items.push_back (Item (layers::ItemKind::Text, "Sun hours"));
    const hud::Layout out = hud.Lay ({ &titled, &right }, At (600.0f, 600.0f));
    ASSERT_GT (out.dock.width, 0.0f);
    const hud::Built& panel = out.panels[1];
    EXPECT_FLOAT_EQ (panel.fraction[0], 1.0f);
    EXPECT_NEAR (panel.offset[0], -panel.width - 16.0f - out.dock.width, 1.0f);
}

// ⚠️ ONE STATE FOR BOTH VIEWS: the host closed in the 3D window is closed in the plan, on the
// tab it was on. And a project closing forgets it all (§8).
TEST (OverlayHudControls, TheViewsShareWhatTheUserDid)
{
    const std::shared_ptr<hud::State> state = hud::NewState ();
    RightDock (state);
    Fresh threeD, plan;
    threeD.engine.UseState (state);
    plan.engine.UseState (state);
    const layers::Panel panel = Titled ();
    const hud::Layout open = threeD.Lay ({ &panel }, At (600.0f, 600.0f));
    const float x = 1200.0f + open.dock.offset[0] + open.dock.width * 0.5f;
    const float y = 400.0f + open.dock.offset[1] + open.dock.height - open.dock.width * 0.5f;
    threeD.Click ({ &panel }, x, y);
    EXPECT_FALSE (plan.engine.Open ());
    EXPECT_TRUE (plan.Lay ({ &panel }, At (600.0f, 600.0f)).host.vertices.empty ());
    hud::ClearState (*state);
    EXPECT_FALSE (plan.Lay ({ &panel }, At (600.0f, 600.0f)).host.vertices.empty ());
    EXPECT_TRUE (plan.engine.Open ());
}

// Through the HUD stream: the host's and the dock's rectangles are the HUD's, the dock's
// over the host's; closed, only the dock's is left, so the model under the host is
// Archicad's again.
TEST (OverlayHudControls, TheHostAndTheDockAreRegions)
{
    layers::Layer layer;
    layer.name = "metrics";
    layer.panels = { Titled () };
    const auto all = [&] () {
        return std::vector<std::shared_ptr<const layers::Layer>> { std::make_shared<const layers::Layer> (layer) };
    };
    Fresh hud;
    auto dockState = hud::NewState ();
    RightDock (dockState);
    hud.engine.UseState (dockState);
    const scene::Scene open = scene::PrepareSceneHud (all (), &hud.engine, 1.0f, At (600.0f, 600.0f));
    ASSERT_EQ (open.regions.size (), 2u);
    EXPECT_EQ (open.regions[0].kind, input::RegionKind::Panel);
    EXPECT_EQ (open.regions[0].layer, "metrics");
    EXPECT_EQ (open.regions[1].kind, input::RegionKind::Dock);
    EXPECT_FLOAT_EQ (open.regions[1].fraction[0], 1.0f);
    EXPECT_FLOAT_EQ (open.regions[1].rect[2], 0.0f) << "flush with the right edge";
    bool edge = false;
    for (const scene::SceneGlyph& glyph : open.glyphs)
        edge = edge || (glyph.position[0] == 1.0f && glyph.position[1] == 0.5f);
    EXPECT_TRUE (edge) << "the dock's triangles";

    layer.panels[0].collapsed = true;
    Fresh other;
    auto closedState = hud::NewState ();
    RightDock (closedState);
    other.engine.UseState (closedState);
    const scene::Scene closed = scene::PrepareSceneHud (all (), &other.engine, 1.0f, At (600.0f, 600.0f));
    ASSERT_EQ (closed.regions.size (), 1u);
    EXPECT_EQ (closed.regions[0].kind, input::RegionKind::Dock);
}

// ---- the Settings tab and what it shows and hides ------------------------------------------

// ⚠️ THE USER, 2026-09-30: a Settings tab instead of A- A+, for the HUD's style and the
// overlay's display. It is the host's last tab. Its text size is a dropdown of the steps:
// chosen, every size of the HUD grows, in both views, the distances from the view's edges
// kept, and the change is said.
TEST (OverlayHudControls, SettingsSetsTheTextSizeInBothViews)
{
    const std::shared_ptr<hud::State> state = hud::NewState ();
    Watched threeD;
    Fresh plan;
    threeD.engine.UseState (state);
    plan.engine.UseState (state);
    const layers::Panel panel = Titled ();
    OpenSettings (threeD, { &panel });
    EXPECT_EQ (threeD.engine.Selected (), "tapioca.settings");
    ASSERT_EQ (threeD.heard.size (), 1u);
    EXPECT_EQ (threeD.heard[0].kind, "panel");
    EXPECT_EQ (threeD.heard[0].text, "Settings");
    const hud::Layout before = plan.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FLOAT_EQ (threeD.engine.FontScale (), 1.0f);
    // The dropdown: the first control down the page's right half.
    const hud::Layout settings = threeD.Lay ({ &panel }, At (600.0f, 600.0f));
    const float x = Left (settings.host) + settings.host.width * 0.75f;
    const std::vector<std::pair<float, float>> controls = Controls (threeD, { &panel }, settings.host, x);
    ASSERT_GE (controls.size (), 1u) << "the text size";
    threeD.Click ({ &panel }, x, Middle (controls[0]));
    const hud::Layout list = threeD.Lay ({ &panel }, At (x, Middle (controls[0])));
    ASSERT_TRUE (list.popup);
    // Its options down the list, 80 % to 200 %: the fourth is 110 %.
    const std::vector<std::pair<float, float>> options = Rows (list.overlay, panel.textRgba);
    ASSERT_EQ (options.size (), 9u);
    float box[4] = {};
    ASSERT_TRUE (Box (list.overlay, panel.textRgba, box));
    threeD.Click ({ &panel }, box[0] + 4.0f, Middle (options[3]));
    EXPECT_FLOAT_EQ (plan.engine.FontScale (), 1.1f);
    ASSERT_EQ (threeD.heard.back ().kind, "fontScale");
    EXPECT_EQ (threeD.heard.back ().text, "110 %");
    const hud::Layout larger = plan.Lay ({ &panel }, At (600.0f, 600.0f));
    // By about the step: glyphs advance by whole pixels, so text does not scale exactly.
    const float grew = larger.host.height / before.host.height;
    EXPECT_GT (grew, 1.04f);
    EXPECT_LT (grew, 1.2f);
    EXPECT_LE (Left (larger.host) + larger.host.width, 1200);
    // Set from Python: the nearest step, and never past the ends.
    threeD.engine.SetFontScale (1.3f);
    EXPECT_FLOAT_EQ (plan.engine.FontScale (), 1.25f);
    threeD.engine.SetFontScale (0.2f);
    EXPECT_FLOAT_EQ (plan.engine.FontScale (), 0.8f);
}

// ⚠️ THE USER, 2026-09-30: on the side tab, a filled or empty circle that shows and hides the
// whole overlay and its HUD without destroying them. Pressed, nothing is laid out but the
// dock, and the change is said; the dock arrow brings the panel back.
TEST (OverlayHudControls, TheDocksCircleHidesTheOverlayAndItsArrowBringsItBack)
{
    Watched hud;
    RightDock (hud.state);
    const layers::Panel panel = Titled ();
    layers::Panel plain;
    plain.anchor = layers::PanelAnchor::BottomLeft;
    plain.items.push_back (Item (layers::ItemKind::Text, "Always here"));
    const hud::Layout open = hud.Lay ({ &panel, &plain }, At (600.0f, 600.0f));
    ASSERT_GT (open.host.height, 0.0f);
    // The circle: the dock's top square, as wide as the tab.
    const float x = 1200.0f + open.dock.offset[0] + open.dock.width * 0.5f;
    const float circle = 400.0f + open.dock.offset[1] + open.dock.width * 0.5f;
    EXPECT_TRUE (hud.Lay ({ &panel, &plain }, At (x, circle)).hand);
    hud.Click ({ &panel, &plain }, x, circle);
    const hud::Layout hidden = hud.Lay ({ &panel, &plain }, At (600.0f, 600.0f));
    EXPECT_FALSE (hud::ContentShown (*hud.state));
    EXPECT_TRUE (hidden.host.vertices.empty ());
    EXPECT_TRUE (hidden.panels[1].vertices.empty ()) << "an untitled panel is the overlay's too";
    EXPECT_GT (hidden.dock.height, 0.0f) << "the way back";
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "overlay");
    EXPECT_EQ (hud.heard[0].text, "hidden");
    // Its arrow: everything back, the panel open.
    hud.Click ({ &panel, &plain }, x, 400.0f + hidden.dock.offset[1] + hidden.dock.height - hidden.dock.width * 0.5f);
    const hud::Layout back = hud.Lay ({ &panel, &plain }, At (600.0f, 600.0f));
    EXPECT_TRUE (hud::ContentShown (*hud.state));
    EXPECT_TRUE (hud.engine.Open ());
    EXPECT_GT (back.host.height, 0.0f);
    EXPECT_GT (back.panels[1].height, 0.0f);
    ASSERT_EQ (hud.heard.size (), 2u);
    EXPECT_EQ (hud.heard[1].text, "shown");
}

// Settings lists every layer drawn in the view, shown or hidden. Unchecked, a layer is hidden:
// its panels are no tab and no window, and the change is said; checked again, they are back.
// The state's revision moves every time, for the renderers to follow.
TEST (OverlayHudControls, SettingsHidesALayerAndItsPanels)
{
    Watched hud;
    hud.engine.SetLayers ({ "hud", "tapioca.storeySlices" });
    const layers::Panel panel = Titled ("Sun study", 0x11AA22FFu);
    OpenSettings (hud, { &panel });
    const hud::Layout settings = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    // The check boxes, at the page's left: Show overlay, Hover readout, then one per layer.
    const float x = Left (settings.host) + 24.0f;
    std::vector<std::pair<float, float>> boxes = Controls (hud, { &panel }, settings.host, x);
    ASSERT_EQ (boxes.size (), 4u); // Show overlay, hover readout, then the two layer checkboxes.
    const uint64_t revision = hud::Revision (*hud.state);
    hud.Click ({ &panel }, x, Middle (boxes[2]));
    EXPECT_FALSE (hud::LayerShown (*hud.state, "hud"));
    EXPECT_TRUE (hud::LayerShown (*hud.state, "tapioca.storeySlices"));
    EXPECT_GT (hud::Revision (*hud.state), revision);
    ASSERT_EQ (hud.heard.back ().kind, "layer");
    EXPECT_EQ (hud.heard.back ().id, "hud");
    EXPECT_EQ (hud.heard.back ().text, "hidden");
    // Its panel is no tab now: Settings alone, in the plain card.
    const hud::Layout without = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_EQ (hud.engine.Selected (), "tapioca.settings");
    boxes = Controls (hud, { &panel }, without.host, x);
    ASSERT_GE (boxes.size (), 3u);
    hud.Click ({ &panel }, x, Middle (boxes[2]));
    EXPECT_TRUE (hud::LayerShown (*hud.state, "hud"));
    EXPECT_EQ (hud.engine.Selected (), "tapioca.settings") << "visibility does not navigate";
}

// A layer with no titled panel -- slices, lines -- still has the dock: its circle and its
// Settings are the way to hide it. The host starts closed then; opened, it shows Settings.
TEST (OverlayHudControls, ALayerWithoutAPanelHasTheDockAndSettings)
{
    Watched hud;
    RightDock (hud.state);
    hud.engine.SetLayers ({ "tapioca.storeySlices" });
    const hud::Layout first = hud.Lay ({}, At (600.0f, 600.0f));
    ASSERT_GT (first.dock.height, 0.0f);
    EXPECT_FALSE (hud.engine.Open ());
    EXPECT_TRUE (first.host.vertices.empty ());
    const float x = 1200.0f + first.dock.offset[0] + first.dock.width * 0.5f;
    hud.Click ({}, x, 400.0f + first.dock.offset[1] + first.dock.height - first.dock.width * 0.5f);
    const hud::Layout open = hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_TRUE (hud.engine.Open ());
    EXPECT_GT (open.host.height, 0.0f);
    EXPECT_EQ (hud.engine.Selected (), "tapioca.settings");
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].title, "Overlay");
    Fresh none;
    EXPECT_FLOAT_EQ (none.Lay ({}, At (600.0f, 600.0f)).dock.width, 0.0f) << "no layer, no dock";
}

// ---- the right-click menu -------------------------------------------------------------------

namespace {

// A right click where the pointer is, as the input layer hands it over, and a frame more for
// what it opened to be drawn.
hud::Layout RightClick (Fresh& hud, const std::vector<const layers::Panel*>& panels, float x, float y)
{
    hud.Lay (panels, At (x, y));
    hud.Lay (panels, At (x, y, { { 1, true } }));
    hud.Lay (panels, At (x, y, { { 1, false } }));
    return hud.Lay (panels, At (x, y));
}

} // namespace

// ⚠️ THE USER, 2026-09-30: right click on the HUD to offer HUD specific options. Released over
// the host, the HUD's menu opens at the pointer and takes the whole view; its first item hides
// the overlay and the menu closes. From the dock, the second shows everything again, the
// panel open. Off the HUD, a right release opens nothing.
TEST (OverlayHudControls, ARightClickOnTheHudOpensItsMenu)
{
    Watched hud;
    RightDock (hud.state);
    const layers::Panel panel = Titled ();
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (RightClick (hud, { &panel }, 600.0f, 600.0f).popup) << "off the HUD";
    // The host's background.
    const float x = Left (open.host) + open.host.width * 0.5f, y = Top (open.host) + open.host.height - 4.0f;
    const hud::Layout menu = RightClick (hud, { &panel }, x, y);
    ASSERT_TRUE (menu.popup);
    std::vector<std::pair<float, float>> items = Rows (menu.overlay, panel.textRgba);
    ASSERT_EQ (items.size (), 6u) << "show overlay, show panel, Settings, hover readout, text size -- reset position "
                                     "disabled";
    float box[4] = {};
    ASSERT_TRUE (Box (menu.overlay, panel.textRgba, box));
    EXPECT_NEAR (box[0], x, 32.0f) << "at the pointer";
    EXPECT_NEAR (items[0].first, y, 32.0f);
    hud.Click ({ &panel }, box[0] + 4.0f, Middle (items[0]));
    EXPECT_FALSE (hud::ContentShown (*hud.state));
    const hud::Layout hidden = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (hidden.popup) << "chosen, the menu closes";
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "overlay");
    // The dock's title, the only thing left.
    const float dx = 1200.0f + hidden.dock.offset[0] + hidden.dock.width * 0.5f;
    const float dy = 400.0f + hidden.dock.offset[1] + hidden.dock.width * 0.5f;
    const hud::Layout again = RightClick (hud, { &panel }, dx, dy);
    ASSERT_TRUE (again.popup);
    EXPECT_FALSE (hud::ContentShown (*hud.state)) << "a right click is not the title's press";
    items = Rows (again.overlay, panel.textRgba);
    ASSERT_EQ (items.size (), 1u) << "the dock context menu only offers its location submenu";
    hud.Click ({ &panel }, 600.0f, 600.0f); // dismiss the context menu
    hud.Click ({ &panel }, dx, Top (hidden.dock) + hidden.dock.height - hidden.dock.width * 0.5f);
    EXPECT_TRUE (hud::ContentShown (*hud.state));
    EXPECT_TRUE (hud.engine.Open ());
    EXPECT_GT (hud.Lay ({ &panel }, At (600.0f, 600.0f)).host.height, 0.0f);
}

// The menu's Settings shows the host on its Settings tab; its text size is a submenu of the
// steps, beside it; its layers another, when there are layers.
TEST (OverlayHudControls, TheMenuOpensSettingsAndSetsTheTextSize)
{
    Watched hud;
    hud.engine.SetLayers ({ "hud" });
    const layers::Panel panel = Titled ();
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    const float x = Left (open.host) + open.host.width * 0.5f, y = Top (open.host) + open.host.height - 4.0f;
    hud::Layout menu = RightClick (hud, { &panel }, x, y);
    std::vector<std::pair<float, float>> items = Rows (menu.overlay, panel.textRgba);
    ASSERT_EQ (items.size (), 7u) << "the actions, layers and reset";
    float box[4] = {};
    ASSERT_TRUE (Box (menu.overlay, panel.textRgba, box));
    hud.Click ({ &panel }, box[0] + 4.0f, Middle (items[2]));
    EXPECT_EQ (hud.engine.Selected (), "tapioca.settings");
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "panel");
    EXPECT_EQ (hud.heard[0].text, "Settings");

    // The text size: pointed at, its submenu opens beside the menu.
    const hud::Layout settings = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    menu = RightClick (hud, { &panel }, Left (settings.host) + settings.host.width * 0.5f,
                       Top (settings.host) + settings.host.height - 4.0f);
    items = Rows (menu.overlay, panel.textRgba);
    ASSERT_EQ (items.size (), 7u);
    ASSERT_TRUE (Box (menu.overlay, panel.textRgba, box));
    float frame[4] = {};
    ASSERT_TRUE (Box (menu.overlay, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u, frame));
    hud.Lay ({ &panel }, At (box[0] + 4.0f, Middle (items[4])));
    const hud::Layout sub = hud.Lay ({ &panel }, At (box[0] + 4.0f, Middle (items[4])));
    const std::vector<std::pair<float, float>> steps = Rows (sub.overlay, panel.textRgba, frame[2]);
    ASSERT_EQ (steps.size (), 9u) << "80 % to 200 %";
    hud.Click ({ &panel }, frame[2] + 24.0f, Middle (steps[3]));
    EXPECT_FLOAT_EQ (hud.engine.FontScale (), 1.1f);
    EXPECT_EQ (hud.heard.back ().kind, "fontScale");
    EXPECT_EQ (hud.heard.back ().text, "110 %");

    // Hover readout, after Settings: turned on and said.
    const hud::Layout after = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    menu = RightClick (hud, { &panel }, Left (after.host) + after.host.width * 0.5f,
                       Top (after.host) + after.host.height - 4.0f);
    items = Rows (menu.overlay, panel.textRgba);
    ASSERT_EQ (items.size (), 7u);
    ASSERT_TRUE (Box (menu.overlay, panel.textRgba, box));
    hud.Click ({ &panel }, box[0] + 4.0f, Middle (items[3]));
    EXPECT_TRUE (hud::HoverMode (*hud.state));
    EXPECT_EQ (hud.heard.back ().kind, "hover");
}

// ---- what the user changed, for Python -------------------------------------------------------

// A section folded in the host's page is said with its panel's key and title; pointing and
// laying out again say nothing.
TEST (OverlayHudControls, ASectionFoldedInTheHostIsSaidOnce)
{
    Watched hud;
    layers::Panel panel = Titled ();
    panel.items.push_back (Item (layers::ItemKind::Section, "Spacing metrics"));
    panel.items.push_back (Item (layers::ItemKind::Row, "Healthcare", "18.3 %"));
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    // The section's row: under the tab row, the swatch and the row. Found by pointing down
    // the page until the hand shows below the tab row.
    float row = 0.0f;
    for (float probeY = Top (open.host) + 75; probeY < Top (open.host) + open.host.height && row == 0.0f;
         probeY += 2.0f)
        if (hud.Lay ({ &panel }, At (Left (open.host) + 60.0f, probeY)).hand)
            row = probeY;
    ASSERT_GT (row, 0.0f) << "the section's row";
    hud.Click ({ &panel }, Left (open.host) + 60.0f, row + 2.0f);
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "section");
    EXPECT_EQ (hud.heard[0].key, "hud#0");
    EXPECT_EQ (hud.heard[0].title, "Area metrics");
    EXPECT_EQ (hud.heard[0].id, "Spacing metrics");
    EXPECT_EQ (hud.heard[0].text, "folded");
    hud.Lay ({ &panel }, At (Left (open.host) + 60.0f, row + 2.0f));
    hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_EQ (hud.heard.size (), 1u);
}

// ---- crisp text ------------------------------------------------------------------------------

// ⚠️ THE USER, 2026-09-30: the light panel's text slightly blurry. Glyphs are rasterised at
// their size and advanced by whole pixels, and a panel is placed where ImGui put it, against
// the anchor rounded as the shader rounds it: on an odd view, a panel in the middle of an
// edge lands on whole pixels, and so does every corner of its text.
TEST (OverlayHudControls, TheTextLandsOnWholePixels)
{
    Fresh hud;
    layers::Panel panel;
    layers::ApplyTheme (panel, layers::PanelTheme::Light);
    panel.anchor = layers::PanelAnchor::Right;
    panel.items.push_back (Item (layers::ItemKind::Text, "Healthcare 18.3 % \xE2\x80\x94 Residential 34.4 %"));
    panel.items.push_back (Item (layers::ItemKind::Text, "Illuminated 7.25 h, shaded 4.75 h"));
    hud::Input input = At (600.0f, 600.0f);
    input.width = 1201.0f;
    input.height = 801.0f;
    const hud::Layout out = hud.Lay ({ &panel }, input);
    const hud::Built& built = out.panels[0];
    ASSERT_FLOAT_EQ (built.fraction[1], 0.5f);
    const float top = std::floor (0.5f * 801.0f + 0.5f) + built.offset[1];
    const float left = std::floor (1.0f * 1201.0f + 0.5f) + built.offset[0];
    EXPECT_FLOAT_EQ (top, std::round (top)) << "the panel's top on a whole pixel";
    EXPECT_FLOAT_EQ (left, std::round (left));
    size_t glyphs = 0;
    for (const hud::Vertex& v : built.vertices) {
        if (v.rgba != panel.textRgba)
            continue;
        ++glyphs;
        EXPECT_NEAR (v.x, std::round (v.x), 1e-3) << "a glyph's corner between pixels";
        EXPECT_NEAR (v.y, std::round (v.y), 1e-3);
    }
    EXPECT_GT (glyphs, 60u);
}

// ⚠️ THE USER'S STAGE 3: hover mode reads out what is under the pointer -- a slice's
// figures -- and tints the item. Its switch is in Settings, said as a "hover" change. Off,
// nothing is shown. ⚠️ THE USER, 2026-10-01: the readout in the ImGui panel, not near the
// pointer -- under the host's page, held while the pointer is on the panel to read it.
TEST (OverlayHudControls, HoverModeReadsOutInThePanelAndTintsTheItem)
{
    Watched hud;
    hud.engine.SetLayers ({ "tapioca.storeySlices" });
    const layers::Panel panel = Titled ("Area metrics");
    hud::Input input = At (600.0f, 500.0f);
    input.hover.picks = true;
    input.hover.active = true;
    input.hover.title = "A-01 F2";
    input.hover.rows = { { "Area", "184.0 mÂ²" } };
    input.hover.tintModel = { 0, 0, 3, 10, 0, 3, 10, 10, 3 }; // the slice's triangle, in model metres
    // The same view with nothing under the pointer.
    hud::Input nothing = At (600.0f, 500.0f);
    nothing.hover.picks = true;

    const hud::Layout off = hud.Lay ({ &panel }, input);
    EXPECT_FALSE (off.hoverTint) << "hover mode is off until it is turned on";

    // On in Settings: the check box after Show overlay.
    OpenSettings (hud, { &panel });
    const hud::Layout settings = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    const float x = Left (settings.host) + 24.0f;
    const std::vector<std::pair<float, float>> boxes = Controls (hud, { &panel }, settings.host, x);
    ASSERT_EQ (boxes.size (), 3u) << "Show overlay, Hover readout, the layer";
    hud.Click ({ &panel }, x, Middle (boxes[1]));
    EXPECT_TRUE (hud::HoverMode (*hud.state));
    ASSERT_EQ (hud.heard.back ().kind, "hover");
    EXPECT_EQ (hud.heard.back ().text, "on");

    // Laid out twice: the host takes its size from the frame before.
    hud.Lay ({ &panel }, nothing);
    const hud::Layout idle = hud.Lay ({ &panel }, nothing);
    hud.Lay ({ &panel }, input);
    const hud::Layout on = hud.Lay ({ &panel }, input);
    EXPECT_TRUE (on.hoverTint) << "the item tinted -- by the view's renderer, in model metres";
    // ⚠️ NOTHING FLOATS: no readout by the pointer, and no tint laid out in view pixels -- it
    // stayed where the item had been while the view orbited (the user, 2026-10-01).
    EXPECT_TRUE (on.overlay.vertices.empty ());
    EXPECT_GT (on.host.height, idle.host.height) << "the slice's title and its row, read out in the panel";
    EXPECT_GT (Rows (on.host, panel.textRgba).size (), Rows (idle.host, panel.textRgba).size ());

    // Gone to the panel to read it: the pointer on the host, what is under it there says
    // nothing -- the readout holds what the pointer was on, and nothing is tinted.
    hud::Input onPanel = nothing;
    onPanel.x = Left (on.host) + 40;
    onPanel.y = Top (on.host) + 110;
    hud.Lay ({ &panel }, onPanel);
    const hud::Layout reading = hud.Lay ({ &panel }, onPanel);
    EXPECT_FALSE (reading.hoverTint);
    EXPECT_FLOAT_EQ (reading.host.height, on.host.height) << "held while the pointer is on the HUD";
    EXPECT_EQ (Rows (reading.host, panel.textRgba).size (), Rows (on.host, panel.textRgba).size ());

    // Off the item again, off the panel: nothing to read out.
    hud.Lay ({ &panel }, nothing);
    EXPECT_FLOAT_EQ (hud.Lay ({ &panel }, nothing).host.height, idle.host.height);
}

TEST (OverlayHudControls, DockFlyoutListsWorkflowsAndClosesAfterSelection)
{
    Watched hud;
    RightDock (hud.state);
    const layers::Panel sun = Titled ("Sun study"), area = Titled ("Area metrics");
    const hud::Layout before = hud.Lay ({ &sun, &area }, At (600.0f, 600.0f));
    const float x = Left (before.dock) + before.dock.width * 0.5f;
    const float y = Top (before.dock) + before.dock.width * 2.5f;
    const hud::Layout choices = hud.Click ({ &sun, &area }, x, y);
    ASSERT_TRUE (choices.popup);
    EXPECT_EQ (hud.engine.Selected (), "hud#0");
    float fill[4] = {};
    ASSERT_TRUE (Box (choices.overlay, sun.accentRgba, fill));
    EXPECT_LT (fill[2] - fill[0], 2.0f * before.dock.width + 2.0f)
        << "a side dock keeps its workflow choices in a narrow vertical strip";
    EXPECT_GT (fill[3] - fill[1], fill[2] - fill[0]) << "the side flyout runs along the dock";
    const size_t roundVertices = std::count_if (choices.overlay.vertices.begin (), choices.overlay.vertices.end (),
                                                [&] (const auto& v) { return v.rgba == sun.accentRgba; });
    EXPECT_GT (roundVertices, 12u) << "rounded buttons draw more than two square rectangles";
    const float choiceX = fill[0] + 10.0f, choiceY = fill[3] - 8.0f;
    const auto hovered = hud.Lay ({ &sun, &area }, At (choiceX, choiceY));
    const auto darken = [] (uint32_t rgba) {
        uint32_t result = rgba & 0xFFu;
        for (int shift : { 8, 16, 24 })
            result |= uint32_t (std::lround (float ((rgba >> shift) & 0xFFu) * 0.85f)) << shift;
        return result;
    };
    ASSERT_TRUE (Box (hovered.overlay, darken (sun.accentRgba), fill)) << "hover darkens the dock fill";
    EXPECT_LE (fill[2] - fill[0], before.dock.width) << "the choice fits within the dock tab's width";
    EXPECT_GT (fill[3] - fill[1], fill[2] - fill[0]) << "the choice label follows the vertical tab";
    EXPECT_GE (fill[0], Left (before.dock) - 1.0f);
    EXPECT_LE (fill[2], Left (before.dock) + before.dock.width + 1.0f);
    EXPECT_NEAR ((fill[0] + fill[2]) * 0.5f, Left (before.dock) + before.dock.width * 0.5f, 1.0f)
        << "the vertical choice is centered across the dock's width";
    hud.Click ({ &sun, &area }, choiceX, choiceY);
    EXPECT_EQ (hud.engine.Selected (), "hud#1");
    EXPECT_FALSE (hud.Lay ({ &sun, &area }, At (600.0f, 600.0f)).popup);
    EXPECT_FALSE (hud.state->criteriaTextActive);
}

TEST (OverlayHudControls, LeftDockFlyoutFollowsTheVerticalTab)
{
    Watched hud;
    hud.state->host.dock.edge = shell::DockEdge::Left;
    hud.state->host.dock.along = 0.5f;
    const layers::Panel sun = Titled ("Sun study"), area = Titled ("Area metrics");
    const hud::Layout before = hud.Lay ({ &sun, &area }, At (600.0f, 600.0f));
    const float titleX = Left (before.dock) + before.dock.width * 0.5f;
    const float titleY = Top (before.dock) + before.dock.width * 2.5f;
    const hud::Layout choices = hud.Click ({ &sun, &area }, titleX, titleY);
    ASSERT_TRUE (choices.popup);
    float fill[4] = {};
    ASSERT_TRUE (Box (choices.overlay, sun.accentRgba, fill));
    EXPECT_LT (fill[2] - fill[0], 2.0f * before.dock.width + 2.0f);
    EXPECT_GT (fill[3] - fill[1], fill[2] - fill[0]);
    const float choiceX = Left (before.dock) + before.dock.width * 0.5f;
    const float choiceY = fill[3] - 8.0f;
    hud.Click ({ &sun, &area }, choiceX, choiceY);
    EXPECT_EQ (hud.engine.Selected (), "hud#1");
    EXPECT_FALSE (hud.Lay ({ &sun, &area }, At (600.0f, 600.0f)).popup);
}

TEST (OverlayHudControls, TopDockFlyoutCentersAHorizontalRow)
{
    Watched hud;
    const layers::Panel sun = Titled ("Sun study"), area = Titled ("Area metrics");
    const auto before = hud.Lay ({ &sun, &area }, At (600, 600));
    const float titleX = Left (before.dock) + before.dock.height * 2.5f;
    const float titleY = Top (before.dock) + before.dock.height * 0.5f;
    const auto choices = hud.Click ({ &sun, &area }, titleX, titleY);
    ASSERT_TRUE (choices.popup);
    float fill[4] = {};
    ASSERT_TRUE (Box (choices.overlay, sun.accentRgba, fill));
    EXPECT_GE (fill[0], Left (before.dock) + before.dock.width);
    EXPECT_GT (fill[2] - fill[0], fill[3] - fill[1]);
    EXPECT_NEAR ((fill[1] + fill[3]) * 0.5f, titleY, 1.0f);
    hud.Click ({ &sun, &area }, fill[2] - 8, titleY);
    EXPECT_EQ (hud.engine.Selected (), "hud#1");
}

// The chip's workflow flyout opens a closed panel; the panel's menu still controls readout.
TEST (OverlayHudControls, DockWorkflowFlyoutOpensPanelAndItsMenuEnablesReadout)
{
    Watched hud;
    RightDock (hud.state);
    hud.engine.SetLayers ({ "tapioca.storeySlices" });
    const layers::Panel panel = Titled ("Area metrics");
    hud.Lay ({ &panel }, At (600.0f, 600.0f));
    hud::SetHudOpen (*hud.state, false);
    const hud::Layout closed = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    ASSERT_FALSE (hud::HudOpen (*hud.state));
    ASSERT_EQ (closed.host.vertices.size (), 0u) << "closed to the dock";
    // The dock's title: the workflow tabs from there.
    const float dx = 1200.0f + closed.dock.offset[0] + closed.dock.width * 0.5f;
    const float dy = Top (closed.dock) + closed.dock.width * 2.5f;
    const hud::Layout menu = hud.Click ({ &panel }, dx, dy);
    ASSERT_TRUE (menu.popup);
    float box[4] = {};
    ASSERT_TRUE (Box (menu.overlay, panel.backgroundRgba, box));
    EXPECT_LT (box[2] - box[0], 2.0f * closed.dock.width + 2.0f)
        << "the closed side dock also opens a vertical workflow strip";
    hud.Click ({ &panel }, box[0] + 10.0f, (box[1] + box[3]) * 0.5f);
    EXPECT_TRUE (hud::HudOpen (*hud.state));
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (open.popup) << "the workflow tabs disappear after selection";
    const float x = Left (open.host) + open.host.width * 0.5f;
    const float y = Top (open.host) + open.host.height - 4.0f;
    const hud::Layout panelMenu = RightClick (hud, { &panel }, x, y);
    ASSERT_TRUE (panelMenu.popup);
    auto items = Rows (panelMenu.overlay, panel.textRgba);
    ASSERT_EQ (items.size (), 7u);
    ASSERT_TRUE (Box (panelMenu.overlay, panel.textRgba, box));
    hud.Click ({ &panel }, box[0] + 4.0f, Middle (items[3]));
    EXPECT_TRUE (hud::HoverMode (*hud.state));
    EXPECT_TRUE (hud::HudOpen (*hud.state)) << "the readout's panel open";
    EXPECT_EQ (hud.heard.back ().kind, "hover");
}

TEST (OverlayHudControls, AReadoutFromATitledLayerRoutesToItsPanelNotSettings)
{
    hud::Hover hover;
    hover.layer = "clearance-heatmap";
    const std::vector<std::string> titled = { "clearance-heatmap#0", "other#0" };
    EXPECT_TRUE (hud::ReadoutOnPage (hover, "clearance-heatmap#0", titled));
    EXPECT_FALSE (hud::ReadoutOnPage (hover, "tapioca.settings", titled));
    EXPECT_FALSE (hud::ReadoutOnPage (hover, "other#0", titled));

    hover.layer = "untitled-overlay";
    EXPECT_TRUE (hud::ReadoutOnPage (hover, "tapioca.settings", titled))
        << "layers without titled panels keep the host footer";
}
