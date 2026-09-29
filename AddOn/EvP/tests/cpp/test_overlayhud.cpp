// ArchViz/OverlayHud: the HUD panels, laid out by the vendored Dear ImGui into plain
// triangles over its own font atlas. A panel in the wrong corner, a key in the wrong
// colour or a glyph sampling an atlas that moved is a picture over Archicad's view, so
// the layout and the atlas protocol are pinned here against the real library.

#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;
namespace scene = geomsrv::archviz::overlayscene;
namespace text = geomsrv::archviz::overlaytext;

namespace {

std::vector<uint8_t> Font ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

hud::Engine& Engine ()
{
    static hud::Engine engine;
    if (!engine.Ready ()) {
        std::string error;
        EXPECT_TRUE (engine.Init (Font (), error)) << error;
    }
    return engine;
}

layers::PanelItem Item (layers::ItemKind kind, const std::string& text = "", const std::string& value = "")
{
    layers::PanelItem item;
    item.kind = kind;
    item.text = text;
    item.value = value;
    return item;
}

layers::Panel Everything ()
{
    layers::Panel panel;
    panel.title = "Sun study \xE2\x80\x94 21 June";
    panel.items.push_back (Item (layers::ItemKind::Row, "GFA", "2633.2 m\xC2\xB2"));
    panel.items.push_back (Item (layers::ItemKind::Row, "Floors", "7"));
    panel.items.push_back (Item (layers::ItemKind::Separator));
    panel.items.push_back (Item (layers::ItemKind::Text, "Pasirinkime n\xC4\x97ra perdang\xC5\xB3"));
    layers::PanelItem swatch = Item (layers::ItemKind::Swatch, "Retail");
    swatch.rgba = 0x12345678u;
    panel.items.push_back (swatch);
    layers::PanelItem progress = Item (layers::ItemKind::Progress, "42 %");
    progress.fraction = 0.42;
    panel.items.push_back (progress);
    layers::PanelItem ramp = Item (layers::ItemKind::Ramp, "Sun hours");
    EXPECT_TRUE (layers::PresetStops ("sunhours", ramp.colormap.stops));
    ramp.colormap.autoRange = false;
    ramp.colormap.min = 0.0;
    ramp.colormap.max = 8.0;
    ramp.unit = "h";
    panel.items.push_back (ramp);
    layers::PanelItem plot = Item (layers::ItemKind::Plot, "hours");
    plot.values = { 1, 3, 2, 5, 8, 6 };
    panel.items.push_back (plot);
    layers::PanelItem table = Item (layers::ItemKind::Table);
    table.columns = { "Block", "Area" };
    table.rows = { { "A", "812" }, { "B", "640" } };
    panel.items.push_back (table);
    return panel;
}

} // namespace

TEST (OverlayHud, EveryItemLaysOutIntoTrianglesOverItsAtlas)
{
    const layers::Panel panel = Everything ();
    layers::Layer layer;
    layer.name = "hud";
    layer.panels = { panel };
    EXPECT_EQ (layers::Validate (layer), "");
    std::vector<hud::Built> built;
    std::string error;
    ASSERT_TRUE (Engine ().Build ({ &panel }, 1.0f, built, error)) << error;
    ASSERT_EQ (built.size (), 1u);
    const hud::Built& out = built[0];
    EXPECT_GT (out.width, 100.0f);
    EXPECT_GT (out.height, 150.0f);
    ASSERT_FALSE (out.vertices.empty ());
    EXPECT_EQ (out.vertices.size () % 3, 0u);
    const auto& pages = Engine ().Pages ();
    bool swatch = false;
    for (const hud::Vertex& v : out.vertices) {
        ASSERT_LT (v.page, pages.size ());
        ASSERT_NE (pages[v.page], nullptr);
        EXPECT_GE (v.u, 0.0f);
        EXPECT_LE (v.u, 1.0f);
        EXPECT_GE (v.x, -1.0f);
        EXPECT_LE (v.x, out.width + 1.0f);
        swatch = swatch || v.rgba == 0x12345678u;
    }
    EXPECT_TRUE (swatch) << "the swatch's colour, as the caller wrote it";
    const text::Page& atlas = *pages[out.vertices.front ().page];
    EXPECT_EQ (atlas.pixels.size (), size_t (atlas.width) * size_t (atlas.height) * 4u);
    EXPECT_NE (atlas.id, 0u);
}

// ⚠️ THE ATLAS GROWS AS IT MEETS GLYPHS, AND THEN HOLDS STILL: a second build of the
// same panel makes no new page, so the overlays upload nothing again.
TEST (OverlayHud, TheSamePanelTwiceMakesNoNewAtlas)
{
    const layers::Panel panel = Everything ();
    std::vector<hud::Built> first, second;
    std::string error;
    ASSERT_TRUE (Engine ().Build ({ &panel }, 1.0f, first, error)) << error;
    const uint32_t versions = Engine ().GetStats ().atlasVersions;
    std::set<uint64_t> ids;
    for (const auto& page : Engine ().Pages ())
        if (page != nullptr)
            ids.insert (page->id);
    ASSERT_TRUE (Engine ().Build ({ &panel }, 1.0f, second, error)) << error;
    EXPECT_EQ (Engine ().GetStats ().atlasVersions, versions);
    std::set<uint64_t> again;
    for (const auto& page : Engine ().Pages ())
        if (page != nullptr)
            again.insert (page->id);
    EXPECT_EQ (ids, again);
    EXPECT_EQ (first[0].vertices.size (), second[0].vertices.size ());
}

TEST (OverlayHud, AFixedWidthHoldsAndTheScaleScales)
{
    layers::Panel panel;
    panel.widthPixels = 300.0f;
    panel.items.push_back (Item (layers::ItemKind::Text, "One line"));
    std::vector<hud::Built> one, two;
    std::string error;
    ASSERT_TRUE (Engine ().Build ({ &panel }, 1.0f, one, error)) << error;
    ASSERT_TRUE (Engine ().Build ({ &panel }, 2.0f, two, error)) << error;
    EXPECT_NEAR (one[0].width, 300.0f, 0.5f);
    EXPECT_NEAR (two[0].width, 600.0f, 0.5f);
    EXPECT_NEAR (two[0].height, 2.0f * one[0].height, 3.0f);
}

TEST (OverlayHud, APanelSitsAtItsAnchorOffsetInwards)
{
    layers::Panel panel;
    panel.offsetPixels[0] = 10.0f;
    panel.offsetPixels[1] = 20.0f;
    float fraction[2] = {}, offset[2] = {};
    panel.anchor = layers::PanelAnchor::TopRight;
    hud::Place (panel, 200.0f, 100.0f, 1.5f, fraction, offset);
    EXPECT_FLOAT_EQ (fraction[0], 1.0f);
    EXPECT_FLOAT_EQ (fraction[1], 0.0f);
    EXPECT_FLOAT_EQ (offset[0], -200.0f - 15.0f);
    EXPECT_FLOAT_EQ (offset[1], 30.0f);
    panel.anchor = layers::PanelAnchor::Center;
    hud::Place (panel, 200.0f, 100.0f, 1.0f, fraction, offset);
    EXPECT_FLOAT_EQ (fraction[0], 0.5f);
    EXPECT_FLOAT_EQ (fraction[1], 0.5f);
    EXPECT_FLOAT_EQ (offset[0], -100.0f + 10.0f);
    EXPECT_FLOAT_EQ (offset[1], -50.0f + 20.0f);
    panel.anchor = layers::PanelAnchor::BottomLeft;
    hud::Place (panel, 200.0f, 100.0f, 1.0f, fraction, offset);
    EXPECT_FLOAT_EQ (fraction[1], 1.0f);
    EXPECT_FLOAT_EQ (offset[0], 10.0f);
    EXPECT_FLOAT_EQ (offset[1], -100.0f - 20.0f);
}

// The panels reach the overlays as glyph quads in a stream of their own: fixed to the
// view, sampling the HUD's pages -- and never in the scene, so a panel that changes
// never sends the scene again.
TEST (OverlayHud, APanelLayerBecomesPlainTextureQuadsFixedToTheView)
{
    layers::Layer layer;
    layer.name = "hud";
    layer.panels = { Everything () };
    layer.panels[0].anchor = layers::PanelAnchor::BottomRight;
    const scene::Scene out =
        scene::PrepareSceneHud ({ std::make_shared<const layers::Layer> (layer) }, &Engine (), 1.25f);
    ASSERT_FALSE (out.glyphs.empty ());
    EXPECT_EQ (out.problems.textsNotLaidOut, 0u) << out.problems.lastError;
    for (const scene::SceneGlyph& glyph : out.glyphs) {
        EXPECT_EQ (glyph.flags, scene::kScreenAnchored | scene::kPlainTexture | scene::kPhysicalPixels);
        EXPECT_FLOAT_EQ (glyph.position[0], 1.0f);
        EXPECT_FLOAT_EQ (glyph.position[1], 1.0f);
        EXPECT_LE (glyph.offset[0], 0.0f); // from the bottom-right corner, inwards
        EXPECT_LE (glyph.offset[1], 0.0f);
    }
    for (const scene::GlyphDraw& draw : out.glyphDraws) {
        ASSERT_LT (draw.page, out.pages.size ());
        EXPECT_NE (out.pages[draw.page], nullptr);
    }
    // The plan's stream is the same panels.
    const scene::Plan plan =
        scene::PreparePlanHud ({ std::make_shared<const layers::Layer> (layer) }, &Engine (), 1.25f);
    EXPECT_EQ (plan.glyphs.size (), out.glyphs.size ());
    // Without the engine the panel is counted, not drawn.
    const scene::Scene none = scene::PrepareSceneHud ({ std::make_shared<const layers::Layer> (layer) }, nullptr, 1.0f);
    EXPECT_TRUE (none.glyphs.empty ());
    EXPECT_EQ (none.problems.textsNotLaidOut, 1u);
    // And the scene never holds a panel.
    const scene::Scene scene = scene::PrepareScene ({ std::make_shared<const layers::Layer> (layer) }, nullptr);
    EXPECT_TRUE (scene.glyphs.empty ());
    EXPECT_EQ (scene.problems.textsNotLaidOut, 0u);
}

TEST (OverlayHud, ValidationNamesWhatAPanelGotWrong)
{
    layers::Layer layer;
    layer.name = "hud";
    layers::Panel panel;
    layers::PanelItem ramp = Item (layers::ItemKind::Ramp);
    ASSERT_TRUE (layers::PresetStops ("viridis", ramp.colormap.stops));
    panel.items = { ramp };
    layer.panels = { panel };
    EXPECT_NE (layers::Validate (layer).find ("needs its min and max"), std::string::npos);
    layer.panels[0].items.assign (201, Item (layers::ItemKind::Separator));
    EXPECT_NE (layers::Validate (layer).find ("at most 200 items"), std::string::npos);
}
