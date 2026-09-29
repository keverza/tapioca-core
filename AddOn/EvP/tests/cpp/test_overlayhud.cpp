// ArchViz/OverlayHud: the HUD panels, laid out by the vendored Dear ImGui into plain
// triangles over its own font atlas. A panel in the wrong corner, a key in the wrong
// colour or a glyph sampling an atlas that moved is a picture over Archicad's view, so
// the layout and the atlas protocol are pinned here against the real library.

#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
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

// ---- the pointer over the HUD (OverlayInput.hpp feeds it) ---------------------------------

namespace {

// A fresh engine per test: what one test folded must not fold the next one's panel.
struct Fresh {
    hud::Engine engine;
    Fresh ()
    {
        std::string error;
        EXPECT_TRUE (engine.Init (Font (), error)) << error;
    }
    hud::Layout Lay (const std::vector<const layers::Panel*>& panels, const hud::Input& input,
                     const std::vector<hud::LegendBar>& legends = {})
    {
        std::vector<std::string> keys;
        for (size_t i = 0; i < panels.size (); ++i)
            keys.push_back ("hud#" + std::to_string (i));
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (engine.Build (panels, keys, 1.0f, input, legends, out, error)) << error;
        return out;
    }
};

hud::Input At (float x, float y, std::vector<hud::Input::Button> buttons = {})
{
    hud::Input input;
    input.width = 1200.0f;
    input.height = 800.0f;
    input.pointer = true;
    input.x = x;
    input.y = y;
    input.buttons = std::move (buttons);
    return input;
}

// A click where the pointer is, as the input layer hands it over: a press, then a release.
void Click (Fresh& hud, const std::vector<const layers::Panel*>& panels, float x, float y)
{
    hud.Lay (panels, At (x, y));
    hud.Lay (panels, At (x, y, { { 0, true } }));
    hud.Lay (panels, At (x, y, { { 0, false } }));
}

layers::Panel Titled ()
{
    layers::Panel panel;
    panel.title = "Area metrics";
    panel.items.push_back (Item (layers::ItemKind::Row, "Site area", "11 214 m\xC2\xB2"));
    panel.items.push_back (Item (layers::ItemKind::Row, "BCR", "20.0 %"));
    return panel;
}

} // namespace

// The pointer on a ramp shows its value there; off it, nothing floats over the panel.
TEST (OverlayHud, APointedRampSaysItsValue)
{
    Fresh hud;
    layers::Panel panel;
    layers::PanelItem ramp = Item (layers::ItemKind::Ramp);
    ASSERT_TRUE (layers::PresetStops ("sunhours", ramp.colormap.stops));
    ramp.colormap.autoRange = false;
    ramp.colormap.min = 0.0;
    ramp.colormap.max = 12.0;
    ramp.widthPixels = 200.0f;
    ramp.unit = "h";
    panel.items = { ramp };
    const hud::Layout away = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (away.overlay.vertices.empty ());
    // The bar starts at the window's padding inside the panel at (16, 16).
    const float left = 16.0f + panel.paddingPixels, top = 16.0f + panel.paddingPixels;
    const hud::Layout over = hud.Lay ({ &panel }, At (left + 150.0f, top + 5.0f));
    EXPECT_FALSE (over.overlay.vertices.empty ()) << "a tooltip over the pointed bar";
    // Laid out again with the pointer where it was, the same pixels.
    const hud::Layout again = hud.Lay ({ &panel }, At (left + 150.0f, top + 5.0f));
    EXPECT_EQ (again.overlay.vertices.size (), over.overlay.vertices.size ());
}

// ⚠️ THE HUD IS COLLAPSIBLE (the user, 2026-09-29): the arrow on a titled panel's title
// bar folds it to that bar, and it stays folded when the layer is set again.
TEST (OverlayHud, TheTitleBarsArrowFoldsThePanelAndItStaysFolded)
{
    Fresh hud;
    const layers::Panel panel = Titled ();
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    ASSERT_EQ (open.panels.size (), 1u);
    EXPECT_FALSE (hud.engine.Collapsed ("hud#0"));
    // The arrow sits at the title bar's start, a frame's padding in: at (16, 16) the
    // panel, 4 and 3 pixels the padding, the title's font 14 x 1.2.
    const float arrowX = 16.0f + 4.0f + 8.0f, arrowY = 16.0f + 3.0f + 8.0f;
    Click (hud, { &panel }, arrowX, arrowY);
    const hud::Layout folded = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (hud.engine.Collapsed ("hud#0"));
    EXPECT_LT (folded.panels[0].height, open.panels[0].height * 0.7f);
    // Set again -- a new panel object under the same key -- it is still folded.
    const layers::Panel republished = Titled ();
    const hud::Layout still = hud.Lay ({ &republished }, At (600.0f, 600.0f));
    EXPECT_NEAR (still.panels[0].height, folded.panels[0].height, 0.5f);
    // And the arrow opens it again.
    Click (hud, { &republished }, arrowX, arrowY);
    const hud::Layout reopened = hud.Lay ({ &republished }, At (600.0f, 600.0f));
    EXPECT_FALSE (hud.engine.Collapsed ("hud#0"));
    EXPECT_NEAR (reopened.panels[0].height, open.panels[0].height, 0.5f);
}

// A panel that starts folded, as its caller asked.
TEST (OverlayHud, APanelStartsFoldedWhenAskedTo)
{
    Fresh hud;
    layers::Panel panel = Titled ();
    const hud::Layout open = hud.Lay ({ &panel }, At (600.0f, 600.0f));
    Fresh other;
    panel.collapsed = true;
    const hud::Layout folded = other.Lay ({ &panel }, At (600.0f, 600.0f));
    EXPECT_TRUE (other.engine.Collapsed ("hud#0"));
    EXPECT_LT (folded.panels[0].height, open.panels[0].height * 0.7f);
}

// A section's chevron folds the items after it, up to the next section.
TEST (OverlayHud, ASectionFoldsItsItems)
{
    Fresh hud;
    layers::Panel panel;
    panel.items.push_back (Item (layers::ItemKind::Section, "GFA", "19 821 m\xC2\xB2"));
    for (const char* use : { "Healthcare", "Hospitality", "Mixeduse", "Residential" })
        panel.items.push_back (Item (layers::ItemKind::Row, use, "3 630 m\xC2\xB2"));
    layers::PanelItem gia = Item (layers::ItemKind::Section, "GIA", "17 839 m\xC2\xB2");
    gia.open = false;
    panel.items.push_back (gia);
    panel.items.push_back (Item (layers::ItemKind::Row, "Healthcare", "3 267 m\xC2\xB2"));
    layers::Layer layer;
    layer.name = "metrics";
    layer.panels = { panel };
    EXPECT_EQ (layers::Validate (layer), "");

    const hud::Layout open = hud.Lay ({ &panel }, At (900.0f, 600.0f));
    bool state = true;
    ASSERT_TRUE (hud.engine.SectionOpen ("hud#0", 0, state));
    EXPECT_TRUE (state);
    ASSERT_TRUE (hud.engine.SectionOpen ("hud#0", 5, state));
    EXPECT_FALSE (state) << "the second section starts folded, as asked";
    // Its header is the first row, a padding inside the panel at (16, 16).
    Click (hud, { &panel }, 16.0f + panel.paddingPixels + 30.0f, 16.0f + panel.paddingPixels + 6.0f);
    const hud::Layout folded = hud.Lay ({ &panel }, At (900.0f, 600.0f));
    ASSERT_TRUE (hud.engine.SectionOpen ("hud#0", 0, state));
    EXPECT_FALSE (state);
    EXPECT_LT (folded.panels[0].height, open.panels[0].height * 0.7f);

    // A section needs its title: it is what the user clicks.
    layer.panels[0].items[0].text.clear ();
    EXPECT_NE (layers::Validate (layer).find ("a section needs its title"), std::string::npos);
}

// A legend's bar -- drawn by the scene, hovered here -- says its value at the pointer.
TEST (OverlayHud, APointedLegendSaysItsValue)
{
    Fresh hud;
    layers::Legend legend;
    ASSERT_TRUE (layers::PresetStops ("viridis", legend.colormap.stops));
    legend.colormap.min = 0.0;
    legend.colormap.max = 100.0;
    legend.unit = "%";
    hud::LegendBar bar;
    bar.legend = &legend;
    bar.rect[0] = 1100.0f;
    bar.rect[1] = 500.0f;
    bar.rect[2] = 1112.0f;
    bar.rect[3] = 660.0f;
    EXPECT_TRUE (hud.Lay ({}, At (600.0f, 600.0f), { bar }).overlay.vertices.empty ());
    const hud::Layout over = hud.Lay ({}, At (1106.0f, 580.0f), { bar });
    ASSERT_FALSE (over.overlay.vertices.empty ());
    // The bar is at the view's right: the tooltip opens towards the middle.
    float right = -FLT_MAX;
    for (const hud::Vertex& v : over.overlay.vertices)
        right = (std::max) (right, v.x);
    EXPECT_LE (right, 1100.0f);
}

// Two layouts that draw the same pixels have the same fingerprint, so the second is not
// uploaded or redrawn; a hover that changes the pixels changes it.
TEST (OverlayHud, TheFingerprintMovesOnlyWithThePixels)
{
    layers::Layer layer;
    layer.name = "hud";
    layers::Panel panel;
    layers::PanelItem ramp = Item (layers::ItemKind::Ramp);
    ASSERT_TRUE (layers::PresetStops ("sunhours", ramp.colormap.stops));
    ramp.colormap.autoRange = false;
    ramp.colormap.min = 0.0;
    ramp.colormap.max = 12.0;
    ramp.widthPixels = 200.0f;
    panel.items = { ramp };
    layer.panels = { panel };
    const std::vector<std::shared_ptr<const layers::Layer>> all = { std::make_shared<const layers::Layer> (layer) };
    Fresh hud;
    hud::Input input = At (600.0f, 600.0f);
    const uint64_t first = scene::Fingerprint (scene::PrepareSceneHud (all, &hud.engine, 1.0f, input));
    const uint64_t second = scene::Fingerprint (scene::PrepareSceneHud (all, &hud.engine, 1.0f, input));
    EXPECT_EQ (first, second);
    input.x = 16.0f + panel.paddingPixels + 100.0f;
    input.y = 16.0f + panel.paddingPixels + 5.0f;
    const uint64_t hovered = scene::Fingerprint (scene::PrepareSceneHud (all, &hud.engine, 1.0f, input));
    EXPECT_NE (hovered, first);
}
