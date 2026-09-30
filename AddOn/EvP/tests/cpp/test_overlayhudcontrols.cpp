// ArchViz/OverlayHud, stage 2: what the user presses on the HUD -- the tint and the hand
// over it, the dock the panels close to, the font size, and the controls whose values the
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

// ---- the dock ------------------------------------------------------------------------------

namespace {

layers::Panel Titled (const char* title = "Area metrics")
{
    layers::Panel panel;
    panel.title = title;
    panel.items.push_back (Item (layers::ItemKind::Row, "Site area", "11 214 m\xC2\xB2"));
    panel.items.push_back (Item (layers::ItemKind::Row, "BCR", "20.0 %"));
    return panel;
}

// The close button's middle on a panel at the view's top-left (16, 16): at the title bar's
// end, a frame's padding in (4, 3), as big as the title's font (14 x 1.2).
float CloseX (const hud::Built& panel)
{
    return 16.0f + panel.width - 4.0f - 8.4f;
}
constexpr float kCloseY = 16.0f + 3.0f + 8.4f;

// The middle of the dock's `k`th tab of `count`, on the 1200 x 800 view: the tabs one
// under another, 4 pixels apart, the text size's row under them as tall as a tab.
void TabAt (const hud::Built& dock, size_t k, size_t count, float& x, float& y)
{
    const float tab = (dock.height - 4.0f * float (count)) / float (count + 1);
    x = 1200.0f + dock.offset[0] + dock.width * 0.5f;
    y = 400.0f + dock.offset[1] + tab * 0.5f + (tab + 4.0f) * float (k);
}

// The middle of the text size's smaller (`larger` false) or larger button, under `count`
// tabs.
void SizeAt (const hud::Built& dock, size_t count, bool larger, float& x, float& y)
{
    const float tab = (dock.height - 4.0f * float (count)) / float (count + 1);
    const float half = (dock.width - 4.0f) * 0.5f;
    x = 1200.0f + dock.offset[0] + (larger ? half + 4.0f + half * 0.5f : half * 0.5f);
    y = 400.0f + dock.offset[1] + (tab + 4.0f) * float (count) + tab * 0.5f;
}

} // namespace

// The close button is pressable: the hand over it, an arrow over the title beside it.
TEST (OverlayHudControls, TheCloseButtonShowsTheHand)
{
    Fresh hud;
    const layers::Panel panel = Titled ();
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (hud.Lay ({ &panel }, At (CloseX (open.panels[0]), kCloseY)).hand);
    EXPECT_FALSE (hud.Lay ({ &panel }, At (16.0f + 30.0f, kCloseY)).hand) << "the title itself presses nothing";
}

// ⚠️ THE USER, 2026-09-29: the collapsed panel is a tab in a list down the view's right
// edge. The close button sends the panel there -- nothing of it left where it was -- its
// tab loses the accent, and a press on the tab brings the panel back as it was. What the
// user did outlives the layer being set again.
TEST (OverlayHudControls, TheCloseButtonDocksThePanelAndItsTabOpensIt)
{
    Fresh hud;
    const layers::Panel panel = Titled ();
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    ASSERT_GT (open.panels[0].height, 30.0f);
    ASSERT_GT (open.dock.width, 0.0f) << "a titled panel has a tab";
    float box[4] = {};
    EXPECT_TRUE (Box (open.dock, panel.accentRgba, box)) << "an open panel's tab is filled with its accent";
    EXPECT_FLOAT_EQ (open.dock.fraction[0], 1.0f);
    EXPECT_FLOAT_EQ (open.dock.fraction[1], 0.5f);
    EXPECT_FLOAT_EQ (open.dock.offset[0], -open.dock.width) << "flush with the view's right edge";

    hud.Click ({ &panel }, CloseX (open.panels[0]), kCloseY);
    const hud::Layout docked = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (hud.engine.Collapsed ("hud#0"));
    EXPECT_TRUE (docked.panels[0].vertices.empty ());
    EXPECT_FLOAT_EQ (docked.panels[0].height, 0.0f);
    EXPECT_FALSE (Box (docked.dock, panel.accentRgba, box)) << "a closed panel's tab is in its card's colours";
    EXPECT_TRUE (Box (docked.dock, panel.backgroundRgba, box));

    // Set again -- a new panel object under the same key -- it is still in the dock.
    const layers::Panel republished = Titled ();
    EXPECT_TRUE (hud.Lay ({ &republished }, At (600.0f, 600.0f)).panels[0].vertices.empty ());

    float x = 0.0f, y = 0.0f;
    TabAt (docked.dock, 0, 1, x, y);
    hud.Click ({ &republished }, x, y);
    const hud::Layout reopened = hud.Lay ({ &republished }, At (600.0f, 600.0f));
    EXPECT_FALSE (hud.engine.Collapsed ("hud#0"));
    EXPECT_NEAR (reopened.panels[0].height, open.panels[0].height, 0.5f);
    EXPECT_NEAR (reopened.panels[0].width, open.panels[0].width, 0.5f);
    // And its tab closes it again.
    hud.Click ({ &republished }, x, y);
    EXPECT_TRUE (hud.engine.Collapsed ("hud#0"));
}

// A panel asked to start collapsed starts in the dock; one without a title has no tab.
TEST (OverlayHudControls, APanelStartsInTheDockWhenAskedToAndAnUntitledOneHasNoTab)
{
    Fresh hud;
    layers::Panel panel = Titled ();
    panel.collapsed = true;
    layers::Panel plain;
    plain.anchor = layers::PanelAnchor::BottomLeft;
    plain.items.push_back (Item (layers::ItemKind::Text, "Always here"));
    const hud::Layout out = hud.Lay ({ &panel, &plain }, At (600.0f, 600.0f));
    EXPECT_TRUE (out.panels[0].vertices.empty ());
    EXPECT_GT (out.panels[1].height, 0.0f);
    EXPECT_GT (out.dock.height, 0.0f);
    Fresh other;
    plain.collapsed = true; // nothing to press to open it: an untitled panel never docks
    const hud::Layout alone = other.Lay ({ &plain }, At (600.0f, 600.0f));
    EXPECT_GT (alone.panels[0].height, 0.0f);
    EXPECT_FLOAT_EQ (alone.dock.width, 0.0f);
    EXPECT_TRUE (alone.dock.vertices.empty ());
}

// Two panels, two tabs down the edge; a panel on the view's right column moves in beside
// the dock rather than under it.
TEST (OverlayHudControls, TheRightColumnMovesInBesideTheDock)
{
    Fresh hud;
    layers::Panel left = Titled ("Area metrics");
    layers::Panel right = Titled ("Sun hours");
    right.anchor = layers::PanelAnchor::TopRight;
    const hud::Layout out = hud.Lay ({ &left, &right }, At (600.0f, 600.0f));
    ASSERT_GT (out.dock.width, 0.0f);
    const hud::Built& panel = out.panels[1];
    EXPECT_FLOAT_EQ (panel.fraction[0], 1.0f);
    EXPECT_NEAR (panel.offset[0], -panel.width - 16.0f - (out.dock.width + 8.0f), 0.5f);
    // Its triangles are where it says it is: its right edge clear of the dock.
    float box[4] = {};
    ASSERT_TRUE (Box (panel, right.backgroundRgba, box));
    EXPECT_LE (1200.0f + panel.offset[0] + box[2], 1200.0f - out.dock.width - 7.5f);
    // The second tab opens and closes the second panel.
    float x = 0.0f, y = 0.0f;
    TabAt (out.dock, 1, 2, x, y);
    hud.Click ({ &left, &right }, x, y);
    EXPECT_TRUE (hud.engine.Collapsed ("hud#1"));
    EXPECT_FALSE (hud.engine.Collapsed ("hud#0"));
}

// ⚠️ ONE STATE FOR BOTH VIEWS: a panel docked in the 3D window is docked in the plan. And
// a project closing forgets it all (§8).
TEST (OverlayHudControls, TheViewsShareWhatTheUserDid)
{
    const std::shared_ptr<hud::State> state = hud::NewState ();
    Fresh threeD, plan;
    threeD.engine.UseState (state);
    plan.engine.UseState (state);
    const layers::Panel panel = Titled ();
    const hud::Layout open = threeD.Lay ({ &panel }, At (600.0f, 600.0f));
    threeD.Click ({ &panel }, CloseX (open.panels[0]), kCloseY);
    EXPECT_TRUE (plan.engine.Collapsed ("hud#0"));
    EXPECT_TRUE (plan.Lay ({ &panel }, At (600.0f, 600.0f)).panels[0].vertices.empty ());
    hud::ClearState (*state);
    EXPECT_FALSE (plan.engine.Collapsed ("hud#0"));
    EXPECT_FALSE (plan.Lay ({ &panel }, At (600.0f, 600.0f)).panels[0].vertices.empty ());
}

// Through the HUD stream: the dock's rectangle is the HUD's, its triangles anchored at the
// view's right edge half-way down; a panel in the dock has no rectangle at all, so the
// model under where it was is Archicad's again.
TEST (OverlayHudControls, TheDockIsARegionAndADockedPanelIsNone)
{
    layers::Layer layer;
    layer.name = "metrics";
    layer.panels = { Titled () };
    const auto all = [&] () {
        return std::vector<std::shared_ptr<const layers::Layer>> { std::make_shared<const layers::Layer> (layer) };
    };
    Fresh hud;
    const scene::Scene open = scene::PrepareSceneHud (all (), &hud.engine, 1.0f, At (600.0f, 600.0f));
    size_t panels = 0, docks = 0;
    for (const input::Region& region : open.regions) {
        panels += region.kind == input::RegionKind::Panel ? 1 : 0;
        docks += region.kind == input::RegionKind::Dock ? 1 : 0;
        if (region.kind == input::RegionKind::Dock) {
            EXPECT_FLOAT_EQ (region.fraction[0], 1.0f);
            EXPECT_FLOAT_EQ (region.fraction[1], 0.5f);
            EXPECT_FLOAT_EQ (region.rect[2], 0.0f) << "flush with the right edge";
        }
    }
    EXPECT_EQ (panels, 1u);
    EXPECT_EQ (docks, 1u);
    bool edge = false;
    for (const scene::SceneGlyph& glyph : open.glyphs)
        edge = edge || (glyph.position[0] == 1.0f && glyph.position[1] == 0.5f);
    EXPECT_TRUE (edge) << "the dock's triangles";

    layer.panels[0].collapsed = true;
    Fresh other;
    const scene::Scene docked = scene::PrepareSceneHud (all (), &other.engine, 1.0f, At (600.0f, 600.0f));
    ASSERT_EQ (docked.regions.size (), 1u);
    EXPECT_EQ (docked.regions[0].kind, input::RegionKind::Dock);
}

// ---- the text size -------------------------------------------------------------------------

// ⚠️ THE USER, 2026-09-29: a control for the HUD's font size. The dock's larger button
// grows every size of the HUD by a step -- in both views -- and leaves the distances from
// the view's edges; the smaller one walks back and stops at the smallest step. Pointed at,
// they say the size they give.
TEST (OverlayHudControls, TheTextSizeButtonsScaleTheWholeHudInBothViews)
{
    const std::shared_ptr<hud::State> state = hud::NewState ();
    Fresh threeD, plan;
    threeD.engine.UseState (state);
    plan.engine.UseState (state);
    const layers::Panel panel = Titled ();
    const hud::Layout before = threeD.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_FLOAT_EQ (threeD.engine.FontScale (), 1.0f);
    float x = 0.0f, y = 0.0f;
    SizeAt (before.dock, 1, true, x, y);
    EXPECT_FALSE (threeD.Lay ({ &panel }, At (x, y)).overlay.vertices.empty ()) << "the size it gives, said";
    threeD.Click ({ &panel }, x, y);
    EXPECT_FLOAT_EQ (plan.engine.FontScale (), 1.1f);
    const hud::Layout larger = plan.Lay ({ &panel }, At (600.0f, 600.0f));
    // By about the step: glyphs advance by whole pixels, so text does not scale exactly.
    const float grew = larger.panels[0].width / before.panels[0].width;
    EXPECT_GT (grew, 1.02f);
    EXPECT_LT (grew, 1.2f);
    EXPECT_NEAR (larger.panels[0].height / before.panels[0].height, 1.1f, 0.06f);
    EXPECT_GT (larger.dock.width, before.dock.width);
    EXPECT_FLOAT_EQ (larger.panels[0].offset[0], 16.0f) << "the distance from the view's edge stays";
    // Four smaller: 1.0, 0.9, 0.8, and 0.8 again.
    for (int k = 0; k < 4; ++k) {
        const hud::Layout now = plan.Lay ({ &panel }, At (600.0f, 600.0f));
        SizeAt (now.dock, 1, false, x, y);
        plan.Click ({ &panel }, x, y);
    }
    EXPECT_FLOAT_EQ (threeD.engine.FontScale (), 0.8f);
    // Set from outside, the nearest step.
    threeD.engine.SetFontScale (1.3f);
    EXPECT_FLOAT_EQ (plan.engine.FontScale (), 1.25f);
}

// ---- what the user changed, for Python -------------------------------------------------------

// ⚠️ THE USER, 2026-09-29: the add-on sends change events to Python. Every change a layout
// makes is said once -- in the layout and to the engine's sink, after ImGui's lock -- with
// the panel's key and title: the close button and a tab dock and open, a section folds, the
// size buttons change the text size. Pointing at things says nothing.
TEST (OverlayHudControls, EveryChangeTheUserMakesIsSaidOnce)
{
    Fresh hud;
    std::vector<hud::Change> heard;
    hud.engine.SetChangeSink ([&heard] (const hud::Change& change) { heard.push_back (change); });
    layers::Panel panel = Titled ();
    panel.items.push_back (Item (layers::ItemKind::Section, "Spacing metrics"));
    panel.items.push_back (Item (layers::ItemKind::Row, "Healthcare", "18.3 %"));
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (open.changes.empty ());

    const hud::Layout closed = hud.Click ({ &panel }, CloseX (open.panels[0]), kCloseY);
    ASSERT_EQ (heard.size (), 1u);
    EXPECT_EQ (heard[0].kind, "dock");
    EXPECT_EQ (heard[0].key, "hud#0");
    EXPECT_EQ (heard[0].title, "Area metrics");
    EXPECT_DOUBLE_EQ (heard[0].value, 1.0);
    EXPECT_EQ (heard[0].text, "docked");
    EXPECT_EQ (closed.changes.size (), 1u) << "the layout that made it says it too";

    float x = 0.0f, y = 0.0f;
    TabAt (closed.dock, 0, 1, x, y);
    hud.Click ({ &panel }, x, y);
    ASSERT_EQ (heard.size (), 2u);
    EXPECT_EQ (heard[1].text, "open");
    EXPECT_DOUBLE_EQ (heard[1].value, 0.0);

    // The section's row: under the title bar and the two rows. Found by pointing down the
    // panel until the hand shows past the rows.
    const hud::Layout reopened = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    float row = 0.0f;
    for (float probeY = 40.0f; probeY < 16.0f + reopened.panels[0].height && row == 0.0f; probeY += 2.0f)
        if (hud.Lay ({ &panel }, At (16.0f + 40.0f, probeY)).hand)
            row = probeY;
    ASSERT_GT (row, 0.0f) << "the section's row";
    hud.Click ({ &panel }, 16.0f + 40.0f, row + 2.0f);
    ASSERT_EQ (heard.size (), 3u);
    EXPECT_EQ (heard[2].kind, "section");
    EXPECT_EQ (heard[2].id, "Spacing metrics");
    EXPECT_EQ (heard[2].item, 2);
    EXPECT_EQ (heard[2].text, "folded");

    const hud::Layout now = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    SizeAt (now.dock, 1, true, x, y);
    hud.Click ({ &panel }, x, y);
    ASSERT_EQ (heard.size (), 4u);
    EXPECT_EQ (heard[3].kind, "fontScale");
    EXPECT_TRUE (heard[3].key.empty ()) << "the HUD's own, no panel's";
    EXPECT_NEAR (heard[3].value, 1.1, 1e-6);
    EXPECT_EQ (heard[3].text, "110 %");

    // Pointing and laying out again say nothing.
    hud.Lay ({ &panel }, At (x, y));
    hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_EQ (heard.size (), 4u);
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
