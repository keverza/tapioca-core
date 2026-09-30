// ArchViz/OverlayHud, stage 2: what the user presses on the HUD -- the tint and the hand
// over it, the floating host and its dock, its Settings, and the controls whose values the
// add-on holds and reports to Python. Laid out by the vendored Dear ImGui, as the add-on
// does, and pressed as the input layer hands the presses over.

#include "hud_fixture.hpp"

#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace hudtest;

namespace input = geomsrv::archviz::overlayinput;
namespace scene = geomsrv::archviz::overlayscene;

namespace {

uint32_t WithAlpha (uint32_t rgba, float factor)
{
    const uint32_t a = uint32_t (std::lround (float (rgba & 0xFFu) * factor));
    return (rgba & 0xFFFFFF00u) | (std::min) (a, 255u);
}

} // namespace

// ⚠️ THE USER, 2026-09-29: a hand over what the HUD can press, and a tint on it. A
// section's row is pressable: pointed at, it is tinted with the panel's accent and the
// layout asks for the hand; over the panel's still parts, and off it, an arrow.
TEST (OverlayHudControls, APressableRowIsTintedAndShowsTheHand)
{
    Fresh hud;
    layers::Panel panel;
    panel.items.push_back (Item (layers::ItemKind::Section, "Spacing metrics"));
    layers::PanelItem gap = Item (layers::ItemKind::Spacing);
    gap.heightPixels = 40.0f;
    panel.items.push_back (gap);
    const uint32_t tint = WithAlpha (panel.accentRgba, 0.16f);
    // The panel at (16, 16), its padding in: the section's row, then the gap under it.
    const float left = 16.0f + panel.paddingPixels, top = 16.0f + panel.paddingPixels;
    const hud::Layout away = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (away.hand);
    EXPECT_FALSE (Drawn (away, tint));
    const hud::Layout row = hud.Lay ({ &panel }, At (left + 30.0f, top + 6.0f));
    EXPECT_TRUE (row.hand);
    EXPECT_TRUE (Drawn (row, tint)) << "the row tinted with the accent";
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
    for (float y = kRowY + 12.0f; y < 16.0f + host.height; y += 2.0f) {
        const bool hand = hud.Lay (panels, At (x, y)).hand;
        if (hand && !in)
            runs.push_back ({ y, y });
        if (hand)
            runs.back ().second = y;
        in = hand;
    }
    return runs;
}

// The rows of one colour's triangles, top to bottom: a list's lines of text.
std::vector<std::pair<float, float>> Rows (const hud::Built& built, uint32_t rgba)
{
    std::vector<std::pair<float, float>> spans;
    for (size_t t = 0; t + 2 < built.vertices.size (); t += 3) {
        if (built.vertices[t].rgba != rgba)
            continue;
        float lo = FLT_MAX, hi = -FLT_MAX;
        for (size_t k = t; k < t + 3; ++k) {
            lo = (std::min) (lo, built.vertices[k].y);
            hi = (std::max) (hi, built.vertices[k].y);
        }
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
void OpenSettings (Fresh& hud, const std::vector<const layers::Panel*>& panels)
{
    const hud::Layout open = hud.Lay (panels, At (600.0f, 600.0f));
    const std::vector<std::pair<float, float>> runs = Presses (hud, panels, open.host);
    ASSERT_GE (runs.size (), 2u);
    hud.Click (panels, Middle (runs[runs.size () - 2]), kRowY);
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
    const std::vector<std::pair<float, float>> runs = Presses (hud, { &sun, &area }, first.host);
    ASSERT_EQ (runs.size (), 4u) << "two tabs, Settings and the close button";
    hud.Click ({ &sun, &area }, Middle (runs[1]), kRowY);
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

// ⚠️ THE USER, 2026-09-30: one tab at the side, its text turned 90 degrees, that opens and
// closes the panel. It stands at the view's right edge, taller than wide, its title running
// down it under the overlay's circle; filled with the accent while the host is open. The close button at the end of the
// host's tab row closes it too, and every open and close is said.
TEST (OverlayHudControls, OneTurnedTabOpensAndClosesTheHost)
{
    Watched hud;
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

    hud.Click ({ &panel }, x, y);
    const hud::Layout closed = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FALSE (hud.engine.Open ());
    EXPECT_TRUE (closed.host.vertices.empty ());
    EXPECT_FALSE (Box (closed.dock, panel.accentRgba, box)) << "in the card's colours while closed";
    hud.Click ({ &panel }, x, y);
    EXPECT_TRUE (hud.engine.Open ());

    // The close button, last along the tab row.
    const hud::Layout again = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    const std::vector<std::pair<float, float>> runs = Presses (hud, { &panel }, again.host);
    ASSERT_EQ (runs.size (), 3u);
    hud.Click ({ &panel }, Middle (runs.back ()), kRowY);
    EXPECT_FALSE (hud.engine.Open ());
    ASSERT_EQ (hud.heard.size (), 3u);
    EXPECT_EQ (hud.heard[0].kind, "hud");
    EXPECT_EQ (hud.heard[0].text, "closed");
    EXPECT_EQ (hud.heard[1].text, "open");
    EXPECT_EQ (hud.heard[2].text, "closed");
    EXPECT_EQ (hud.heard[2].title, "Area metrics");
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
TEST (OverlayHudControls, TheHostStaysWhereItIsDragged)
{
    Watched hud;
    const layers::Panel panel = Titled ();
    const hud::Layout before = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    // Its background: in the padding under its last item.
    const float x = 16.0f + before.host.width * 0.5f, y = 16.0f + before.host.height - 4.0f;
    EXPECT_FALSE (hud.Lay ({ &panel }, At (x, y)).hand) << "the background: nothing to press";
    hud.Lay ({ &panel }, At (x, y, { { 0, true } }));
    hud.Lay ({ &panel }, At (x + 400.0f, y + 250.0f));
    hud.Lay ({ &panel }, At (x + 800.0f, y + 500.0f));
    const hud::Layout dropped = hud.Lay ({ &panel }, At (x + 800.0f, y + 500.0f, { { 0, false } }));
    // Anchored at the bottom-right now, where it was left.
    EXPECT_FLOAT_EQ (dropped.host.fraction[0], 1.0f);
    EXPECT_FLOAT_EQ (dropped.host.fraction[1], 1.0f);
    EXPECT_NEAR (1200.0f + dropped.host.offset[0], 16.0f + 800.0f, 1.0f);
    EXPECT_NEAR (800.0f + dropped.host.offset[1], 16.0f + 500.0f, 1.0f);
    // A bigger view: as far from its right and bottom edges.
    hud::Input bigger = At (100.0f, 100.0f);
    bigger.width = 1600.0f;
    bigger.height = 1000.0f;
    const hud::Layout grown = hud.Lay ({ &panel }, bigger);
    EXPECT_FLOAT_EQ (grown.host.offset[0], dropped.host.offset[0]);
    EXPECT_FLOAT_EQ (grown.host.offset[1], dropped.host.offset[1]);
    // A smaller one than it was left in: still wholly inside.
    hud::Input smaller = At (10.0f, 10.0f);
    smaller.width = 500.0f;
    smaller.height = 400.0f;
    const hud::Layout shrunk = hud.Lay ({ &panel }, smaller);
    EXPECT_GE (500.0f + shrunk.host.offset[0], 0.0f);
    EXPECT_GE (400.0f + shrunk.host.offset[1], 0.0f);
    EXPECT_TRUE (hud.heard.empty ()) << "a move is not a change to report";
}

// A panel without a title on the view's right column moves in beside the dock.
TEST (OverlayHudControls, TheRightColumnMovesInBesideTheDock)
{
    Fresh hud;
    const layers::Panel titled = Titled ();
    layers::Panel right;
    right.anchor = layers::PanelAnchor::TopRight;
    right.items.push_back (Item (layers::ItemKind::Text, "Sun hours"));
    const hud::Layout out = hud.Lay ({ &titled, &right }, At (600.0f, 600.0f));
    ASSERT_GT (out.dock.width, 0.0f);
    const hud::Built& panel = out.panels[1];
    EXPECT_FLOAT_EQ (panel.fraction[0], 1.0f);
    EXPECT_NEAR (panel.offset[0], -panel.width - 16.0f - (out.dock.width + 6.0f), 1.0f);
}

// ⚠️ ONE STATE FOR BOTH VIEWS: the host closed in the 3D window is closed in the plan, on the
// tab it was on. And a project closing forgets it all (§8).
TEST (OverlayHudControls, TheViewsShareWhatTheUserDid)
{
    const std::shared_ptr<hud::State> state = hud::NewState ();
    Fresh threeD, plan;
    threeD.engine.UseState (state);
    plan.engine.UseState (state);
    const layers::Panel panel = Titled ();
    const hud::Layout open = threeD.Lay ({ &panel }, At (600.0f, 600.0f));
    const float x = 1200.0f + open.dock.offset[0] + open.dock.width * 0.5f;
    const float y = 400.0f + open.dock.offset[1] + open.dock.height * 0.5f;
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
    const float x = 16.0f + settings.host.width * 0.75f;
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
    EXPECT_NEAR (larger.host.offset[0], 16.0f, 0.5f) << "the distance from the view's edge stays";
    // Set from Python: the nearest step, and never past the ends.
    threeD.engine.SetFontScale (1.3f);
    EXPECT_FLOAT_EQ (plan.engine.FontScale (), 1.25f);
    threeD.engine.SetFontScale (0.2f);
    EXPECT_FLOAT_EQ (plan.engine.FontScale (), 0.8f);
}

// ⚠️ THE USER, 2026-09-30: on the side tab, a filled or empty circle that shows and hides the
// whole overlay and its HUD without destroying them. Pressed, nothing is laid out but the
// dock, and the change is said; the dock's title brings everything back, the panel open.
TEST (OverlayHudControls, TheDocksCircleHidesTheOverlayAndItsTitleBringsItBack)
{
    Watched hud;
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
    // Its title: everything back, the panel open.
    hud.Click ({ &panel, &plain }, x, 400.0f + hidden.dock.offset[1] + hidden.dock.height * 0.6f);
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
    // The check boxes, at the page's left: Show overlay, then one per layer.
    const float x = 16.0f + 24.0f;
    std::vector<std::pair<float, float>> boxes = Controls (hud, { &panel }, settings.host, x);
    ASSERT_EQ (boxes.size (), 3u);
    const uint64_t revision = hud::Revision (*hud.state);
    hud.Click ({ &panel }, x, Middle (boxes[1]));
    EXPECT_FALSE (hud::LayerShown (*hud.state, "hud"));
    EXPECT_TRUE (hud::LayerShown (*hud.state, "tapioca.storeySlices"));
    EXPECT_GT (hud::Revision (*hud.state), revision);
    ASSERT_EQ (hud.heard.back ().kind, "layer");
    EXPECT_EQ (hud.heard.back ().id, "hud");
    EXPECT_EQ (hud.heard.back ().text, "hidden");
    // Its panel is no tab now: Settings alone, in the plain card.
    const hud::Layout without = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_EQ (Presses (hud, { &panel }, without.host).size (), 2u) << "Settings and the close button";
    boxes = Controls (hud, { &panel }, without.host, x);
    ASSERT_EQ (boxes.size (), 3u);
    hud.Click ({ &panel }, x, Middle (boxes[1]));
    EXPECT_TRUE (hud::LayerShown (*hud.state, "hud"));
    EXPECT_EQ (Presses (hud, { &panel }, hud.Lay ({ &panel }, At (600.0f, 600.0f)).host).size (), 3u);
}

// A layer with no titled panel -- slices, lines -- still has the dock: its circle and its
// Settings are the way to hide it. The host starts closed then; opened, it shows Settings.
TEST (OverlayHudControls, ALayerWithoutAPanelHasTheDockAndSettings)
{
    Watched hud;
    hud.engine.SetLayers ({ "tapioca.storeySlices" });
    const hud::Layout first = hud.Lay ({}, At (600.0f, 600.0f));
    ASSERT_GT (first.dock.height, 0.0f);
    EXPECT_FALSE (hud.engine.Open ());
    EXPECT_TRUE (first.host.vertices.empty ());
    const float x = 1200.0f + first.dock.offset[0] + first.dock.width * 0.5f;
    hud.Click ({}, x, 400.0f + first.dock.offset[1] + first.dock.height * 0.6f);
    const hud::Layout open = hud.Lay ({}, At (600.0f, 600.0f));
    EXPECT_TRUE (hud.engine.Open ());
    EXPECT_GT (open.host.height, 0.0f);
    EXPECT_EQ (hud.engine.Selected (), "tapioca.settings");
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].title, "Overlay");
    Fresh none;
    EXPECT_FLOAT_EQ (none.Lay ({}, At (600.0f, 600.0f)).dock.width, 0.0f) << "no layer, no dock";
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
    for (float probeY = kRowY + 14.0f; probeY < 16.0f + open.host.height && row == 0.0f; probeY += 2.0f)
        if (hud.Lay ({ &panel }, At (16.0f + 60.0f, probeY)).hand)
            row = probeY;
    ASSERT_GT (row, 0.0f) << "the section's row";
    hud.Click ({ &panel }, 16.0f + 60.0f, row + 2.0f);
    ASSERT_EQ (hud.heard.size (), 1u);
    EXPECT_EQ (hud.heard[0].kind, "section");
    EXPECT_EQ (hud.heard[0].key, "hud#0");
    EXPECT_EQ (hud.heard[0].title, "Area metrics");
    EXPECT_EQ (hud.heard[0].id, "Spacing metrics");
    EXPECT_EQ (hud.heard[0].text, "folded");
    hud.Lay ({ &panel }, At (16.0f + 60.0f, row + 2.0f));
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
