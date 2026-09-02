#include "ArchViz/SceneTextAtlas.hpp"
#include "ArchViz/SceneTextLayout.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadFont ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

} // namespace

TEST (SceneTextAtlas, DecodesValidAndMalformedUtf8Deterministically)
{
    EXPECT_EQ (geomsrv::archviz::DecodeSceneTextUtf8 ("A\xC4\x85\xC2\xB0"),
               (std::vector<uint32_t> { 'A', 0x0105, 0x00B0 }));
    EXPECT_EQ (geomsrv::archviz::DecodeSceneTextUtf8 ("\xF0\x28\x8C\x28"),
               (std::vector<uint32_t> { 0xFFFD, '(', 0xFFFD, '(' }));
    EXPECT_EQ (geomsrv::archviz::DecodeSceneTextUtf8 ("\xC0\xAF"), (std::vector<uint32_t> { 0xFFFD, 0xFFFD }));
}

TEST (SceneTextAtlas, BuildsBoundedRepeatableLinearMtsdfSeed)
{
    const std::vector<uint8_t> font = ReadFont ();
    ASSERT_FALSE (font.empty ());
    geomsrv::archviz::SceneTextAtlas first;
    geomsrv::archviz::SceneTextAtlas second;
    std::string firstError, secondError;
    ASSERT_TRUE (first.Build (font.data (), font.size (), firstError)) << firstError;
    ASSERT_TRUE (second.Build (font.data (), font.size (), secondError)) << secondError;
    EXPECT_GT (first.GlyphCount (), 100u);
    EXPECT_LE (first.Width (), geomsrv::archviz::SceneTextAtlas::kMaximumDimension);
    EXPECT_LE (first.Height (), geomsrv::archviz::SceneTextAtlas::kMaximumDimension);
    EXPECT_EQ (first.Width (), second.Width ());
    EXPECT_EQ (first.Height (), second.Height ());
    EXPECT_EQ (first.Pixels (), second.Pixels ());
    EXPECT_NE (first.Find (0x0105), nullptr);   // Lithuanian a-ogonek
    EXPECT_NE (first.Find (0x00B2), nullptr);   // square unit suffix
    EXPECT_NE (first.Find (0x2192), nullptr);   // right arrow
    EXPECT_NE (first.Find (0x10FFFF), nullptr); // replacement fallback

    geomsrv::archviz::SceneTextShaper shaper;
    ASSERT_TRUE (shaper.Init (font.data (), font.size (), firstError)) << firstError;
    geomsrv::archviz::SceneTextGlyphRun shaped;
    ASSERT_TRUE (shaper.Shape ("office a\xCC\x81", geomsrv::archviz::SceneTextDirection::Auto, shaped, firstError))
        << firstError;
    for (const auto& glyph : shaped.glyphs)
        EXPECT_NE (first.FindGlyph (glyph.glyphIndex), nullptr);
}

TEST (SceneTextAtlas, RejectsInvalidFontData)
{
    const uint8_t invalid[] = { 0, 1, 2, 3 };
    geomsrv::archviz::SceneTextAtlas atlas;
    std::string error;
    EXPECT_FALSE (atlas.Build (invalid, sizeof (invalid), error));
    EXPECT_FALSE (error.empty ());
}
