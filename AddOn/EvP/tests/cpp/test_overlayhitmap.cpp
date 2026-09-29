// ArchViz/OverlayHitMap: where the HUD is on the view and whose a mouse gesture is. A
// wrong answer either selects the element under a panel the user meant to click, or
// swallows a click the user meant for Archicad -- both reported as the overlay being
// broken, and neither visible in any log. So the routing is pinned here, and the
// regions are checked against where the legend and the panel are actually drawn.

#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <iterator>

namespace input = geomsrv::archviz::overlayinput;
namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;
namespace scene = geomsrv::archviz::overlayscene;
namespace text = geomsrv::archviz::overlaytext;

namespace {

input::Region Box (float fx, float fy, float left, float top, float right, float bottom, bool logical = false)
{
    input::Region region;
    region.fraction[0] = fx;
    region.fraction[1] = fy;
    region.rect[0] = left;
    region.rect[1] = top;
    region.rect[2] = right;
    region.rect[3] = bottom;
    region.logical = logical;
    return region;
}

input::Event Press (input::Button button, uint32_t held)
{
    return { input::EventKind::Press, button, held };
}

input::Event Release (input::Button button, uint32_t held)
{
    return { input::EventKind::Release, button, held };
}

input::Event Move (uint32_t held = 0)
{
    return { input::EventKind::Move, input::Button::Left, held };
}

constexpr uint32_t kLeft = input::Bit (input::Button::Left);
constexpr uint32_t kRight = input::Bit (input::Button::Right);
constexpr uint32_t kMiddle = input::Bit (input::Button::Middle);

std::vector<uint8_t> Font ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

text::Engine& TextEngine ()
{
    static text::Engine engine;
    if (!engine.Ready ()) {
        std::string error;
        EXPECT_TRUE (engine.Init (Font (), error, text::Engine::SmallSeedText ())) << error;
    }
    return engine;
}

hud::Engine& HudEngine ()
{
    static hud::Engine engine;
    if (!engine.Ready ()) {
        std::string error;
        EXPECT_TRUE (engine.Init (Font (), error)) << error;
    }
    return engine;
}

} // namespace

TEST (OverlayHitMap, ARegionIsFoundWhereItIsAnchoredAndTheTopmostWins)
{
    input::HitMap map;
    map.regions.push_back (Box (1.0f, 1.0f, -200.0f, -100.0f, -10.0f, -10.0f));  // bottom-right, physical
    map.regions.push_back (Box (0.0f, 0.0f, 16.0f, 16.0f, 116.0f, 66.0f, true)); // top-left, logical
    map.regions.push_back (Box (1.0f, 1.0f, -60.0f, -60.0f, -20.0f, -20.0f));    // over the first
    map.dpiScale = 1.5f;
    EXPECT_EQ (map.Hit (1000.0f - 150.0f, 800.0f - 50.0f, 1000.0f, 800.0f), 0);
    EXPECT_EQ (map.Hit (1000.0f - 30.0f, 800.0f - 30.0f, 1000.0f, 800.0f), 2); // the later is on top
    EXPECT_EQ (map.Hit (1000.0f - 5.0f, 800.0f - 50.0f, 1000.0f, 800.0f), -1); // the margin is Archicad's
    // A logical rectangle is scaled as the vertex shader scales its offsets: 16..116 is 24..174.
    EXPECT_EQ (map.Hit (20.0f, 30.0f, 1000.0f, 800.0f), -1);
    EXPECT_EQ (map.Hit (170.0f, 90.0f, 1000.0f, 800.0f), 1);
    EXPECT_EQ (map.Hit (180.0f, 90.0f, 1000.0f, 800.0f), -1);
    // The same anchors in a resized view.
    EXPECT_EQ (map.Hit (500.0f - 150.0f, 400.0f - 50.0f, 500.0f, 400.0f), 0);
}

// ⚠️ THE REQUIREMENT: a click on the HUD never reaches Archicad, and neither does the
// release -- or Archicad would see half a click.
TEST (OverlayHitMap, APressOnTheHudIsTheHudsUntilItsRelease)
{
    input::Router router;
    EXPECT_EQ (router.Decide (Press (input::Button::Left, kLeft), true), input::Route::Take);
    EXPECT_EQ (router.GetOwner (), input::Owner::Hud);
    // Dragged off the panel -- a slider taken past its end -- it is still the HUD's.
    EXPECT_EQ (router.Decide (Move (kLeft), false), input::Route::Take);
    EXPECT_EQ (router.Decide (Release (input::Button::Left, 0), false), input::Route::Take);
    EXPECT_EQ (router.GetOwner (), input::Owner::None);
    EXPECT_EQ (router.Decide (Move (), false), input::Route::Pass);
    // A right click on it too: no context menu under a panel.
    EXPECT_EQ (router.Decide (Press (input::Button::Right, kRight), true), input::Route::Take);
    EXPECT_EQ (router.Decide (Release (input::Button::Right, 0), true), input::Route::Take);
}

// ⚠️ A wall drawn across a panel still draws: the gesture began in the view.
TEST (OverlayHitMap, APressInTheViewIsArchicadsEvenAcrossTheHud)
{
    input::Router router;
    EXPECT_EQ (router.Decide (Press (input::Button::Left, kLeft), false), input::Route::Pass);
    EXPECT_EQ (router.GetOwner (), input::Owner::Host);
    EXPECT_EQ (router.Decide (Move (kLeft), true), input::Route::Pass);
    EXPECT_EQ (router.Decide (Release (input::Button::Left, 0), true), input::Route::Pass);
    EXPECT_EQ (router.GetOwner (), input::Owner::None);
}

// Over the HUD a bare move is the HUD's -- no pre-selection or info tag under a panel --
// and anywhere else Archicad's: hovering never blocks modelling.
TEST (OverlayHitMap, ABareMoveIsTheHudsOnlyOverIt)
{
    input::Router router;
    EXPECT_EQ (router.Decide (Move (), true), input::Route::Take);
    EXPECT_EQ (router.Decide (Move (), false), input::Route::Pass);
    EXPECT_EQ (router.GetOwner (), input::Owner::None);
}

TEST (OverlayHitMap, NavigationIsNeverTaken)
{
    input::Router router;
    EXPECT_EQ (router.Decide ({ input::EventKind::Wheel, input::Button::Left, 0 }, true), input::Route::Pass);
    // A pan begun on a panel pans, and the moves of it pass.
    EXPECT_EQ (router.Decide (Press (input::Button::Middle, kMiddle), true), input::Route::Pass);
    EXPECT_EQ (router.Decide (Move (kMiddle), true), input::Route::Pass);
    EXPECT_EQ (router.Decide (Release (input::Button::Middle, 0), true), input::Route::Pass);
    EXPECT_EQ (router.Decide (Move (), true), input::Route::Take);
}

// The button came up outside every window: the first move with nothing held ends the
// gesture, rather than leaving the HUD holding every move after it.
TEST (OverlayHitMap, AGestureWhoseReleaseWasLostEndsAtTheNextMove)
{
    input::Router router;
    router.Decide (Press (input::Button::Left, kLeft), true);
    EXPECT_EQ (router.Decide (Move (0), false), input::Route::Pass);
    EXPECT_EQ (router.GetOwner (), input::Owner::None);
    // And a release whose press was never seen is Archicad's: it may be waiting for it.
    EXPECT_EQ (router.Decide (Release (input::Button::Left, 0), true), input::Route::Pass);
}

// A second button joins the gesture the first began, and the gesture ends with the last.
TEST (OverlayHitMap, TheGestureEndsWithItsLastButton)
{
    input::Router router;
    EXPECT_EQ (router.Decide (Press (input::Button::Left, kLeft), true), input::Route::Take);
    EXPECT_EQ (router.Decide (Press (input::Button::Right, kLeft | kRight), false), input::Route::Take);
    EXPECT_EQ (router.Decide (Release (input::Button::Right, kLeft), false), input::Route::Take);
    EXPECT_EQ (router.GetOwner (), input::Owner::Hud);
    EXPECT_EQ (router.Decide (Release (input::Button::Left, 0), false), input::Route::Take);
    EXPECT_EQ (router.GetOwner (), input::Owner::None);
}

// A peeked message comes back to be removed: it gets the same verdict and moves nothing.
TEST (OverlayHitMap, APeekDecidesTheSameAndMovesNothing)
{
    input::Router router;
    EXPECT_EQ (router.Preview (Press (input::Button::Left, kLeft), true), input::Route::Take);
    EXPECT_EQ (router.GetOwner (), input::Owner::None);
    EXPECT_EQ (router.Decide (Press (input::Button::Left, kLeft), true), input::Route::Take);
    EXPECT_EQ (router.Preview (Release (input::Button::Left, 0), false), input::Route::Take);
    EXPECT_EQ (router.GetOwner (), input::Owner::Hud);
}

// A legend's region is its box, anchored as it is drawn: the corner of the view, its
// offsets in logical pixels -- the very numbers its panel's fill is drawn at.
TEST (OverlayHitMap, ALegendsRegionIsTheBoxItIsDrawnIn)
{
    layers::Layer layer;
    layer.name = "sun";
    layers::Legend legend;
    legend.title = "Sun hours";
    legend.colormap.min = 0.0;
    legend.colormap.max = 12.0;
    ASSERT_TRUE (layers::PresetStops ("sunhours", legend.colormap.stops));
    legend.corner = layers::Corner::BottomRight;
    layer.legends = { legend, legend };
    layer.legends[1].corner = layers::Corner::TopLeft;
    const scene::Scene drawn = scene::PrepareScene ({ std::make_shared<const layers::Layer> (layer) }, &TextEngine ());
    ASSERT_EQ (drawn.regions.size (), 2u);
    const input::Region& region = drawn.regions[0];
    EXPECT_EQ (region.kind, input::RegionKind::Legend);
    EXPECT_EQ (region.layer, "sun");
    EXPECT_EQ (region.item, 0u);
    EXPECT_TRUE (region.logical);
    EXPECT_FLOAT_EQ (region.fraction[0], 1.0f);
    EXPECT_FLOAT_EQ (region.fraction[1], 1.0f);
    EXPECT_FLOAT_EQ (region.rect[2], -legend.offsetPixels[0]); // its right edge, the offset in from the corner
    EXPECT_FLOAT_EQ (region.rect[3], -legend.offsetPixels[1]);
    // The box's panel is the first fill, drawn at exactly these offsets.
    ASSERT_FALSE (drawn.fillDraws.empty ());
    float low[2] = { FLT_MAX, FLT_MAX }, high[2] = { -FLT_MAX, -FLT_MAX };
    for (uint32_t v = drawn.fillDraws[0].first; v < drawn.fillDraws[0].first + drawn.fillDraws[0].count; ++v)
        for (int k = 0; k < 2; ++k) {
            low[k] = (std::min) (low[k], drawn.fills[v].offset[k]);
            high[k] = (std::max) (high[k], drawn.fills[v].offset[k]);
        }
    EXPECT_FLOAT_EQ (low[0], region.rect[0]);
    EXPECT_FLOAT_EQ (low[1], region.rect[1]);
    EXPECT_FLOAT_EQ (high[0], region.rect[2]);
    EXPECT_FLOAT_EQ (high[1], region.rect[3]);
    EXPECT_EQ (drawn.regions[1].item, 1u);
    EXPECT_FLOAT_EQ (drawn.regions[1].fraction[0], 0.0f);
}

// A panel's region is the rectangle its triangles cover, in physical pixels.
TEST (OverlayHitMap, APanelsRegionHoldsItsTriangles)
{
    layers::Layer layer;
    layer.name = "metrics";
    layers::Panel panel;
    panel.title = "Area metrics";
    layers::PanelItem row;
    row.kind = layers::ItemKind::Row;
    row.text = "GFA";
    row.value = "19 821 m\xC2\xB2";
    panel.items = { row };
    panel.anchor = layers::PanelAnchor::Right;
    layer.panels = { layers::Panel (), panel };
    const scene::Scene drawn =
        scene::PrepareSceneHud ({ std::make_shared<const layers::Layer> (layer) }, &HudEngine (), 1.5f);
    ASSERT_EQ (drawn.regions.size (), 2u);
    const input::Region& region = drawn.regions[1];
    EXPECT_EQ (region.kind, input::RegionKind::Panel);
    EXPECT_EQ (region.layer, "metrics");
    EXPECT_EQ (region.item, 1u);
    EXPECT_FALSE (region.logical);
    EXPECT_FLOAT_EQ (region.fraction[0], 1.0f);
    EXPECT_FLOAT_EQ (region.fraction[1], 0.5f);
    EXPECT_GT (region.rect[2], region.rect[0]);
    EXPECT_GT (region.rect[3], region.rect[1]);
    // ImGui's anti-aliased fringe reaches half a pixel past the window it measured.
    const float fringe = 0.51f;
    for (const scene::SceneGlyph& glyph : drawn.glyphs) {
        if (glyph.position[0] != 1.0f)
            continue; // the other panel's, at the top-left
        EXPECT_GE (glyph.offset[0], region.rect[0] - fringe);
        EXPECT_LE (glyph.offset[0], region.rect[2] + fringe);
        EXPECT_GE (glyph.offset[1], region.rect[1] - fringe);
        EXPECT_LE (glyph.offset[1], region.rect[3] + fringe);
    }
}
