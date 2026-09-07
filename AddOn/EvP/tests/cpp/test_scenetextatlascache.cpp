#include "ArchViz/SceneTextAtlasCache.hpp"
#include "ArchViz/SceneTextLayout.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>
#include <vector>

namespace {

namespace archviz = geomsrv::archviz;

std::vector<uint8_t> ReadAtlasCacheFont ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

std::vector<uint32_t> ShapeGlyphIds (const std::vector<uint8_t>& font, const char* text)
{
    archviz::SceneTextShaper shaper;
    std::string error;
    EXPECT_TRUE (shaper.Init (font.data (), font.size (), error)) << error;
    archviz::SceneTextGlyphRun run;
    EXPECT_TRUE (shaper.Shape (text, archviz::SceneTextDirection::Auto, run, error)) << error;
    std::vector<uint32_t> ids;
    for (const auto& glyph : run.glyphs)
        ids.push_back (glyph.glyphIndex);
    return ids;
}

std::shared_ptr<const archviz::SceneTextAtlasPage> WaitReady (archviz::SceneTextAtlasCache& cache)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        if (auto page = cache.TakeReady ())
            return page;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return nullptr;
}

} // namespace

TEST (SceneTextAtlasCache, DeduplicatesAndPublishesCopiedFontResultsAsynchronously)
{
    std::vector<uint8_t> font = ReadAtlasCacheFont ();
    ASSERT_FALSE (font.empty ());
    const std::vector<uint32_t> ids = ShapeGlyphIds (font, "atlas cache");
    archviz::SceneTextAtlasCache cache;
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;
    font.clear ();
    font.shrink_to_fit ();

    ASSERT_TRUE (cache.Request (ids));
    ASSERT_TRUE (cache.Request (ids));
    const auto page = WaitReady (cache);
    ASSERT_NE (page, nullptr);
    EXPECT_EQ (page->GlyphIds ().size (), std::set<uint32_t> (ids.begin (), ids.end ()).size ());
    auto stats = cache.Stats ();
    EXPECT_EQ (stats.hits, 1u);
    EXPECT_EQ (stats.misses, 1u);
    EXPECT_EQ (stats.generatedPages, 1u);
    EXPECT_EQ (stats.generatedGlyphs, page->GlyphIds ().size ());
    EXPECT_EQ (stats.stagingPages, 0u);
    EXPECT_EQ (stats.stagingBytes, 0u);

    std::vector<uint32_t> overlap = ids;
    overlap.push_back (ShapeGlyphIds (ReadAtlasCacheFont (), "z").front ());
    ASSERT_TRUE (cache.Request (ids));
    ASSERT_TRUE (cache.Request (overlap));
    EXPECT_EQ (cache.Stats ().misses, 3u);
    ASSERT_NE (WaitReady (cache), nullptr);
    ASSERT_NE (WaitReady (cache), nullptr);

    ASSERT_TRUE (cache.Request (ids));
    ASSERT_NE (WaitReady (cache), nullptr);
    EXPECT_EQ (cache.Stats ().misses, 4u);
}

TEST (SceneTextAtlasCache, EnforcesRequestAndOutstandingGlyphBounds)
{
    const std::vector<uint8_t> font = ReadAtlasCacheFont ();
    archviz::SceneTextAtlasCache cache;
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;
    EXPECT_FALSE (cache.Request ({}));
    std::vector<uint32_t> tooMany (65);
    for (uint32_t index = 0; index < tooMany.size (); ++index)
        tooMany[index] = index;
    EXPECT_FALSE (cache.Request (tooMany));

    for (uint32_t page = 0; page < 4; ++page) {
        std::vector<uint32_t> ids;
        for (uint32_t offset = 0; offset < 64; ++offset)
            ids.push_back (page * 64 + offset);
        ASSERT_TRUE (cache.Request (ids));
    }
    EXPECT_EQ (cache.Stats ().pending, archviz::SceneTextAtlasCache::kMaximumPendingGlyphs);
    EXPECT_FALSE (cache.Request ({ 1000 }));
    EXPECT_EQ (cache.Stats ().rejected, 3u);
}

TEST (SceneTextAtlasCache, HoldsAtMostTwoCompletedStagingPages)
{
    const std::vector<uint8_t> font = ReadAtlasCacheFont ();
    archviz::SceneTextAtlasCache cache;
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;
    ASSERT_TRUE (cache.Request (ShapeGlyphIds (font, "first page")));
    ASSERT_TRUE (cache.Request (ShapeGlyphIds (font, "second page")));
    ASSERT_TRUE (cache.Request (ShapeGlyphIds (font, "third page")));

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    archviz::SceneTextAtlasCacheStats stats;
    do {
        stats = cache.Stats ();
        if (stats.stagingPages == archviz::SceneTextAtlasCache::kMaximumStagingPages)
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    } while (std::chrono::steady_clock::now () < deadline);
    EXPECT_EQ (stats.stagingPages, archviz::SceneTextAtlasCache::kMaximumStagingPages);
    EXPECT_EQ (stats.generatedPages, archviz::SceneTextAtlasCache::kMaximumStagingPages);

    ASSERT_NE (cache.TakeReady (), nullptr);
    ASSERT_NE (WaitReady (cache), nullptr);
    ASSERT_NE (WaitReady (cache), nullptr);
    EXPECT_EQ (cache.Stats ().generatedPages, 3u);
}

TEST (SceneTextAtlasCache, StopsAndJoinsWithWorkOutstanding)
{
    const std::vector<uint8_t> font = ReadAtlasCacheFont ();
    const std::vector<uint32_t> ids = ShapeGlyphIds (font, "shutdown");
    archviz::SceneTextAtlasCache cache;
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;
    ASSERT_TRUE (cache.Request (ids));
    cache.Stop ();
    cache.Stop ();
    const auto stats = cache.Stats ();
    EXPECT_FALSE (stats.running);
    EXPECT_EQ (stats.pending, 0u);
    EXPECT_EQ (stats.stagingPages, 0u);
    EXPECT_EQ (stats.stagingBytes, 0u);
    EXPECT_FALSE (cache.Request (ids));
}
