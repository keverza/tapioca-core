// ArchViz/OverlayText: the overlays' labels, laid out once around their anchor with
// the bundled font. A wrong quad is a label in the wrong place over Archicad's view,
// so the placement rules are pinned here against real shaping, not a stub.

#include "ArchViz/OverlayText.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>

namespace text = geomsrv::archviz::overlaytext;
namespace layers = geomsrv::archviz::overlaylayers;

namespace {

std::vector<uint8_t> ReadFont ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

// One engine for the file: the seed atlas is the slow part, and building it once is
// also what production does.
text::Engine& SharedEngine ()
{
    static text::Engine engine;
    if (!engine.Ready ()) {
        std::string error;
        EXPECT_TRUE (engine.Init (ReadFont (), error)) << error;
    }
    return engine;
}

text::Label Lay (const std::string& utf8, float size, layers::Align align, layers::Baseline baseline)
{
    text::Label label;
    std::string error;
    EXPECT_TRUE (SharedEngine ().Layout (utf8, size, align, baseline, label, error)) << error;
    return label;
}

} // namespace

TEST (OverlayText, TheSeedIsPageZeroAndItsPixelsAreRgba)
{
    text::Engine& engine = SharedEngine ();
    ASSERT_TRUE (engine.Ready ());
    ASSERT_GE (engine.Pages ().size (), 1u);
    const text::Page& seed = *engine.Pages ()[0];
    EXPECT_GT (seed.width, 0);
    EXPECT_GT (seed.height, 0);
    EXPECT_EQ (seed.pixels.size (), size_t (seed.width) * size_t (seed.height) * 4u);
    EXPECT_NE (seed.id, 0u);
}

TEST (OverlayText, AnEngineWithoutAFontRefusesToLayOut)
{
    text::Engine engine;
    text::Label label;
    std::string error;
    EXPECT_FALSE (engine.Layout ("x", 12.0f, layers::Align::Left, layers::Baseline::Alphabetic, label, error));
    EXPECT_FALSE (error.empty ());
    EXPECT_FALSE (engine.Init ({}, error));
}

// ⚠️ MIDDLE CENTRES THE INK EXACTLY; CENTER CENTRES THE ADVANCE. A label's ink box
// is not its advance box -- a trailing glyph's side bearing is advance without ink --
// so centring horizontally is judged within a fraction of the size, vertically exactly.
TEST (OverlayText, CenterMiddlePutsTheAnchorInsideTheInk)
{
    const float size = 20.0f;
    const text::Label label = Lay ("Area 12.5 m\xC2\xB2", size, layers::Align::Center, layers::Baseline::Middle);
    ASSERT_FALSE (label.quads.empty ());
    EXPECT_NEAR ((label.left + label.right) * 0.5f, 0.0f, 0.15f * size);
    EXPECT_NEAR ((label.top + label.bottom) * 0.5f, 0.0f, 1e-3f);
    EXPECT_LT (label.left, 0.0f);
    EXPECT_GT (label.right, 0.0f);
    EXPECT_EQ (label.replaced, 0u);
}

TEST (OverlayText, TopAndBottomBaselinesPutTheInkEdgeOnTheAnchor)
{
    const text::Label top = Lay ("Hg", 16.0f, layers::Align::Left, layers::Baseline::Top);
    const text::Label bottom = Lay ("Hg", 16.0f, layers::Align::Left, layers::Baseline::Bottom);
    EXPECT_NEAR (top.top, 0.0f, 1e-3f);
    EXPECT_GT (top.bottom, 0.0f);
    EXPECT_NEAR (bottom.bottom, 0.0f, 1e-3f);
    EXPECT_LT (bottom.top, 0.0f);
}

TEST (OverlayText, LeftAndRightAlignFromTheAnchor)
{
    const text::Label left = Lay ("Room", 16.0f, layers::Align::Left, layers::Baseline::Middle);
    const text::Label right = Lay ("Room", 16.0f, layers::Align::Right, layers::Baseline::Middle);
    // A side bearing of a glyph or two, never more.
    EXPECT_GT (left.left, -0.2f * 16.0f);
    EXPECT_LT (left.left, 0.3f * 16.0f);
    EXPECT_GT (left.right, 16.0f);
    EXPECT_LT (right.right, 0.2f * 16.0f);
    EXPECT_LT (right.left, -16.0f);
    EXPECT_NEAR (left.right - left.left, right.right - right.left, 1e-3f);
}

TEST (OverlayText, SizeScalesTheLayoutLinearly)
{
    const text::Label small = Lay ("12.50", 10.0f, layers::Align::Center, layers::Baseline::Middle);
    const text::Label large = Lay ("12.50", 30.0f, layers::Align::Center, layers::Baseline::Middle);
    ASSERT_EQ (small.quads.size (), large.quads.size ());
    for (size_t i = 0; i < small.quads.size (); ++i) {
        EXPECT_NEAR (large.quads[i].left, small.quads[i].left * 3.0f, 1e-3f);
        EXPECT_NEAR (large.quads[i].bottom, small.quads[i].bottom * 3.0f, 1e-3f);
        EXPECT_FLOAT_EQ (large.quads[i].u0, small.quads[i].u0);
    }
}

TEST (OverlayText, QuadsSampleInsideTheirPage)
{
    const text::Label label =
        Lay ("\xC5\xA0iauli\xC5\xB3 g. 5", 14.0f, layers::Align::Left, layers::Baseline::Alphabetic);
    ASSERT_FALSE (label.quads.empty ());
    for (const text::Quad& quad : label.quads) {
        EXPECT_LT (quad.left, quad.right);
        EXPECT_LT (quad.top, quad.bottom);
        for (const float uv : { quad.u0, quad.u1, quad.v0, quad.v1 }) {
            EXPECT_GE (uv, 0.0f);
            EXPECT_LE (uv, 1.0f);
        }
        EXPECT_LT (quad.page, SharedEngine ().Pages ().size ());
    }
    EXPECT_EQ (label.replaced, 0u);
}

// ⚠️ A GLYPH THE SEED LACKS GETS A PAGE, ONCE. Greek is not in the seed; the first
// label that needs it adds a page, and the second finds it there.
TEST (OverlayText, AGlyphOutsideTheSeedGetsItsOwnPageOnce)
{
    text::Engine& engine = SharedEngine ();
    const size_t before = engine.Pages ().size ();
    const text::Label first = Lay ("\xCE\xA9\xCF\x88", 18.0f, layers::Align::Left, layers::Baseline::Middle);
    const size_t after = engine.Pages ().size ();
    EXPECT_EQ (after, before + 1);
    ASSERT_EQ (first.quads.size (), 2u);
    for (const text::Quad& quad : first.quads)
        EXPECT_EQ (quad.page, uint32_t (after - 1));
    EXPECT_EQ (first.replaced, 0u);
    Lay ("\xCF\x88\xCE\xA9", 18.0f, layers::Align::Left, layers::Baseline::Middle);
    EXPECT_EQ (engine.Pages ().size (), after);
    const text::Page& page = *engine.Pages ().back ();
    EXPECT_EQ (page.pixels.size (), size_t (page.width) * size_t (page.height) * 4u);
    EXPECT_NE (page.id, engine.Pages ()[0]->id);
}

TEST (OverlayText, ASpaceAdvancesWithoutAQuad)
{
    const text::Label joined = Lay ("ab", 16.0f, layers::Align::Left, layers::Baseline::Alphabetic);
    const text::Label spaced = Lay ("a b", 16.0f, layers::Align::Left, layers::Baseline::Alphabetic);
    EXPECT_EQ (joined.quads.size (), 2u);
    EXPECT_EQ (spaced.quads.size (), 2u);
    EXPECT_GT (spaced.right, joined.right);
}
