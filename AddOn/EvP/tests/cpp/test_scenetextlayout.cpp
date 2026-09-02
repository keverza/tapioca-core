#include "ArchViz/SceneTextLayout.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <vector>

namespace {

std::vector<uint8_t> ReadShapingFont ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

geomsrv::archviz::SceneTextGlyphRun
Shape (geomsrv::archviz::SceneTextShaper& shaper, const std::string& text,
       geomsrv::archviz::SceneTextDirection direction = geomsrv::archviz::SceneTextDirection::Auto)
{
    geomsrv::archviz::SceneTextGlyphRun run;
    std::string error;
    EXPECT_TRUE (shaper.Shape (text, direction, run, error)) << error;
    return run;
}

} // namespace

TEST (SceneTextLayout, RejectsInvalidFontAndUseBeforeInitialization)
{
    geomsrv::archviz::SceneTextShaper shaper;
    geomsrv::archviz::SceneTextGlyphRun run;
    std::string error;
    EXPECT_FALSE (shaper.Shape ("text", geomsrv::archviz::SceneTextDirection::Auto, run, error));
    const uint8_t invalid[] = { 0, 1, 2, 3 };
    EXPECT_FALSE (shaper.Init (invalid, sizeof (invalid), error));
}

TEST (SceneTextLayout, ShapesLithuanianLigaturesAndCombiningMarksDeterministically)
{
    const std::vector<uint8_t> font = ReadShapingFont ();
    ASSERT_FALSE (font.empty ());
    geomsrv::archviz::SceneTextShaper shaper;
    std::string error;
    ASSERT_TRUE (shaper.Init (font.data (), font.size (), error)) << error;

    const auto lithuanian = Shape (shaper, "Ąžuolų 12 m²");
    const auto repeated = Shape (shaper, "Ąžuolų 12 m²");
    ASSERT_EQ (lithuanian.glyphs.size (), repeated.glyphs.size ());
    EXPECT_FLOAT_EQ (lithuanian.advance, repeated.advance);
    for (size_t index = 0; index < lithuanian.glyphs.size (); ++index) {
        EXPECT_EQ (lithuanian.glyphs[index].glyphIndex, repeated.glyphs[index].glyphIndex);
        EXPECT_EQ (lithuanian.glyphs[index].cluster, repeated.glyphs[index].cluster);
        EXPECT_FLOAT_EQ (lithuanian.glyphs[index].xAdvance, repeated.glyphs[index].xAdvance);
    }

    const auto ligature = Shape (shaper, "office");
    EXPECT_LT (ligature.glyphs.size (), 6u);
    const auto decomposed = Shape (shaper, "a\xCC\x81");
    const auto precomposed = Shape (shaper, "\xC3\xA1");
    EXPECT_EQ (decomposed.glyphs.size (), precomposed.glyphs.size ());
    EXPECT_NEAR (decomposed.advance, precomposed.advance, 1e-6f);
}

TEST (SceneTextLayout, PreservesUtf8ClustersDirectionAndReplacement)
{
    const std::vector<uint8_t> font = ReadShapingFont ();
    geomsrv::archviz::SceneTextShaper shaper;
    std::string error;
    ASSERT_TRUE (shaper.Init (font.data (), font.size (), error)) << error;

    const auto rightToLeft = Shape (shaper, "abc", geomsrv::archviz::SceneTextDirection::RightToLeft);
    ASSERT_EQ (rightToLeft.glyphs.size (), 3u);
    EXPECT_EQ (rightToLeft.direction, geomsrv::archviz::SceneTextDirection::RightToLeft);
    EXPECT_GT (rightToLeft.glyphs.front ().cluster, rightToLeft.glyphs.back ().cluster);

    const auto malformed = Shape (shaper, "\xF0\x28\x8C\x28");
    const auto replacement = Shape (shaper, "\xEF\xBF\xBD");
    ASSERT_FALSE (replacement.glyphs.empty ());
    size_t replacementCount = 0;
    for (const auto& glyph : malformed.glyphs)
        replacementCount += glyph.glyphIndex == replacement.glyphs.front ().glyphIndex ? 1u : 0u;
    EXPECT_EQ (replacementCount, 2u);
}
