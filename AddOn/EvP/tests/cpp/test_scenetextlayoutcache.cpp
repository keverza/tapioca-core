#include "ArchViz/SceneTextLayoutCache.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace archviz = geomsrv::archviz;

std::vector<uint8_t> ReadCacheFont ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

std::shared_ptr<const archviz::SceneTextGlyphRun>
WaitFor (archviz::SceneTextLayoutCache& cache, const std::string& text,
         archviz::SceneTextDirection direction = archviz::SceneTextDirection::Auto)
{
    std::string error;
    auto run = cache.WaitFor (text, direction, error);
    EXPECT_NE (run, nullptr) << error;
    return run;
}

} // namespace

TEST (SceneTextLayoutCache, DeduplicatesRequestsAndPublishesImmutableRuns)
{
    const auto font = ReadCacheFont ();
    ASSERT_FALSE (font.empty ());
    archviz::SceneTextLayoutCache cache (8);
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;

    cache.FindOrRequest ("Ąžuolų office");
    cache.FindOrRequest ("Ąžuolų office");
    const auto first = WaitFor (cache, "Ąžuolų office");
    const auto second = cache.FindOrRequest ("Ąžuolų office");

    ASSERT_NE (first, nullptr);
    EXPECT_EQ (first, second);
    EXPECT_FALSE (first->glyphs.empty ());
    const auto stats = cache.Stats ();
    EXPECT_EQ (stats.misses, 1u);
    EXPECT_GE (stats.hits, 2u);
    EXPECT_EQ (stats.entries, 1u);
    EXPECT_EQ (stats.pending, 0u);
}

TEST (SceneTextLayoutCache, DirectionIsPartOfTheKey)
{
    const auto font = ReadCacheFont ();
    archviz::SceneTextLayoutCache cache (4);
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;

    const auto left = WaitFor (cache, "abc", archviz::SceneTextDirection::LeftToRight);
    const auto right = WaitFor (cache, "abc", archviz::SceneTextDirection::RightToLeft);

    ASSERT_NE (left, nullptr);
    ASSERT_NE (right, nullptr);
    EXPECT_EQ (left->direction, archviz::SceneTextDirection::LeftToRight);
    EXPECT_EQ (right->direction, archviz::SceneTextDirection::RightToLeft);
    EXPECT_EQ (cache.Stats ().misses, 2u);
}

TEST (SceneTextLayoutCache, EvictsLeastRecentlyUsedReadyRunWithoutInvalidatingReaders)
{
    const auto font = ReadCacheFont ();
    archviz::SceneTextLayoutCache cache (2);
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;

    const auto heldA = WaitFor (cache, "A");
    const auto heldB = WaitFor (cache, "B");
    ASSERT_NE (cache.FindOrRequest ("A"), nullptr);
    ASSERT_NE (WaitFor (cache, "C"), nullptr);
    const uint64_t missesBeforeB = cache.Stats ().misses;

    EXPECT_EQ (cache.FindOrRequest ("B"), nullptr);
    EXPECT_EQ (cache.Stats ().misses, missesBeforeB + 1);
    ASSERT_NE (heldA, nullptr);
    ASSERT_NE (heldB, nullptr);
    EXPECT_FALSE (heldB->glyphs.empty ());
    EXPECT_EQ (cache.Stats ().entries, 2u);
}

TEST (SceneTextLayoutCache, RejectsUnboundedInputAndStopsIdempotently)
{
    const auto font = ReadCacheFont ();
    archviz::SceneTextLayoutCache cache (2);
    std::string error;
    ASSERT_TRUE (cache.Start (font.data (), font.size (), error)) << error;

    EXPECT_EQ (cache.FindOrRequest (std::string (4097, 'x')), nullptr);
    EXPECT_EQ (cache.Stats ().rejected, 1u);
    cache.Stop ();
    cache.Stop ();
    EXPECT_FALSE (cache.Stats ().running);
    EXPECT_EQ (cache.FindOrRequest ("after stop"), nullptr);
}
