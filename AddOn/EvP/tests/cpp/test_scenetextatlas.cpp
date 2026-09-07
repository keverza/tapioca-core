#include "ArchViz/SceneTextAtlas.hpp"
#include "ArchViz/SceneTextLayout.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
    geomsrv::archviz::SceneTextShaper shaper;
    ASSERT_TRUE (shaper.Init (font.data (), font.size (), firstError)) << firstError;
    geomsrv::archviz::SceneTextGlyphRun seedRun;
    ASSERT_TRUE (shaper.Shape (geomsrv::archviz::SceneTextSeedText (), geomsrv::archviz::SceneTextDirection::Auto,
                               seedRun, firstError))
        << firstError;
    ASSERT_TRUE (first.Build (font.data (), font.size (), seedRun, firstError)) << firstError;
    ASSERT_TRUE (second.Build (font.data (), font.size (), seedRun, secondError)) << secondError;
    EXPECT_GT (first.GlyphCount (), 100u);
    EXPECT_LE (first.Width (), geomsrv::archviz::SceneTextAtlas::kMaximumDimension);
    EXPECT_LE (first.Height (), geomsrv::archviz::SceneTextAtlas::kMaximumDimension);
    EXPECT_EQ (first.Width (), second.Width ());
    EXPECT_EQ (first.Height (), second.Height ());
    EXPECT_EQ (first.Pixels (), second.Pixels ());
    EXPECT_NE (first.Find (0x0105), nullptr);   // Lithuanian a-ogonek
    EXPECT_NE (first.Find (0x00E9), nullptr);   // common Western European e-acute
    EXPECT_NE (first.Find (0x00B2), nullptr);   // square unit suffix
    EXPECT_NE (first.Find (0x2192), nullptr);   // right arrow
    EXPECT_NE (first.Find (0x10FFFF), nullptr); // replacement fallback
    EXPECT_EQ (first.FindGlyphExact (0xFFFFFFFFu), nullptr);
    EXPECT_NE (first.FindGlyph (0xFFFFFFFFu), nullptr); // existing fallback remains

    geomsrv::archviz::SceneTextGlyphRun shaped;
    ASSERT_TRUE (shaper.Shape ("office a\xCC\x81", geomsrv::archviz::SceneTextDirection::Auto, shaped, firstError))
        << firstError;
    for (const auto& glyph : shaped.glyphs)
        EXPECT_NE (first.FindGlyph (glyph.glyphIndex), nullptr);
}

TEST (SceneTextAtlas, GeneratesCanonicalBoundedDynamicPages)
{
    const std::vector<uint8_t> font = ReadFont ();
    ASSERT_FALSE (font.empty ());
    geomsrv::archviz::SceneTextShaper shaper;
    std::string error;
    ASSERT_TRUE (shaper.Init (font.data (), font.size (), error)) << error;
    geomsrv::archviz::SceneTextGlyphRun run;
    ASSERT_TRUE (shaper.Shape ("dynamic atlas", geomsrv::archviz::SceneTextDirection::Auto, run, error)) << error;
    std::vector<uint32_t> forward;
    for (const auto& glyph : run.glyphs)
        forward.push_back (glyph.glyphIndex);
    std::vector<uint32_t> reversed = forward;
    std::reverse (reversed.begin (), reversed.end ());
    reversed.insert (reversed.end (), forward.begin (), forward.end ());

    const auto first = geomsrv::archviz::GenerateSceneTextAtlasPage (font.data (), font.size (), forward, error);
    ASSERT_NE (first, nullptr) << error;
    const auto second = geomsrv::archviz::GenerateSceneTextAtlasPage (font.data (), font.size (), reversed, error);
    ASSERT_NE (second, nullptr) << error;
    EXPECT_EQ (first->GlyphIds (), second->GlyphIds ());
    EXPECT_TRUE (std::is_sorted (first->GlyphIds ().begin (), first->GlyphIds ().end ()));
    EXPECT_EQ (first->Width (), second->Width ());
    EXPECT_EQ (first->Height (), second->Height ());
    EXPECT_EQ (first->Pixels (), second->Pixels ());
    EXPECT_LE (first->Width (), geomsrv::archviz::SceneTextAtlasPage::kMaximumDimension);
    EXPECT_LE (first->Height (), geomsrv::archviz::SceneTextAtlasPage::kMaximumDimension);
    for (uint32_t glyphId : first->GlyphIds ())
        EXPECT_NE (first->FindGlyphExact (glyphId), nullptr);
    EXPECT_EQ (first->FindGlyphExact (0xFFFFFFFFu), nullptr);
}

TEST (SceneTextAtlas, RejectsUnboundedDynamicPageRequests)
{
    const std::vector<uint8_t> font = ReadFont ();
    std::string error;
    EXPECT_EQ (geomsrv::archviz::GenerateSceneTextAtlasPage (font.data (), font.size (), {}, error), nullptr);
    std::vector<uint32_t> tooMany (65);
    for (uint32_t index = 0; index < tooMany.size (); ++index)
        tooMany[index] = index;
    EXPECT_EQ (geomsrv::archviz::GenerateSceneTextAtlasPage (font.data (), font.size (), tooMany, error), nullptr);
}

TEST (SceneTextAtlas, RejectsInvalidFontData)
{
    const uint8_t invalid[] = { 0, 1, 2, 3 };
    geomsrv::archviz::SceneTextAtlas atlas;
    geomsrv::archviz::SceneTextGlyphRun seedRun;
    std::string error;
    EXPECT_FALSE (atlas.Build (invalid, sizeof (invalid), seedRun, error));
    EXPECT_FALSE (error.empty ());
}
