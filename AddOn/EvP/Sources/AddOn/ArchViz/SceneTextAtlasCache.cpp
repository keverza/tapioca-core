#include "ArchViz/SceneTextAtlasCache.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace geomsrv::archviz {
namespace {

using GlyphKey = std::vector<uint32_t>;

GlyphKey Normalize (const std::vector<uint32_t>& glyphIds)
{
    GlyphKey key = glyphIds;
    std::sort (key.begin (), key.end ());
    key.erase (std::unique (key.begin (), key.end ()), key.end ());
    return key;
}

struct ReadyPage {
    GlyphKey key;
    std::shared_ptr<const SceneTextAtlasPage> page;
};

} // namespace

struct SceneTextAtlasCache::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<GlyphKey> queue;
    std::deque<ReadyPage> ready;
    std::set<GlyphKey> active;
    std::set<uint32_t> failedGlyphs;
    std::map<uint32_t, size_t> pendingGlyphs;
    std::thread worker;
    bool running = false;
    bool stopping = false;
    size_t stagingBytes = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t rejected = 0;
    uint64_t failures = 0;
    uint64_t generatedPages = 0;
    uint64_t generatedGlyphs = 0;
    uint64_t generationMicroseconds = 0;

    void RemovePending (const GlyphKey& key)
    {
        for (uint32_t glyphId : key) {
            auto found = pendingGlyphs.find (glyphId);
            if (found != pendingGlyphs.end () && --found->second == 0)
                pendingGlyphs.erase (found);
        }
    }

    void Run (std::shared_ptr<const std::vector<uint8_t>> fontBytes)
    {
        for (;;) {
            GlyphKey key;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait (lock,
                           [this] { return stopping || (!queue.empty () && ready.size () < kMaximumStagingPages); });
                if (stopping)
                    return;
                key = std::move (queue.front ());
                queue.pop_front ();
            }

            std::string error;
            const auto started = std::chrono::steady_clock::now ();
            auto page = GenerateSceneTextAtlasPage (fontBytes->data (), fontBytes->size (), key, error);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - started);
            {
                std::lock_guard<std::mutex> lock (mutex);
                generationMicroseconds += static_cast<uint64_t> (elapsed.count ());
                if (stopping)
                    return;
                if (page != nullptr) {
                    stagingBytes += page->Pixels ().size ();
                    generatedGlyphs += page->GlyphIds ().size ();
                    ++generatedPages;
                    ready.push_back ({ key, std::move (page) });
                }
                else {
                    RemovePending (key);
                    active.erase (key);
                    failedGlyphs.insert (key.begin (), key.end ());
                    ++failures;
                }
            }
        }
    }
};

SceneTextAtlasCache::SceneTextAtlasCache () : impl_ (new Impl)
{
}

SceneTextAtlasCache::~SceneTextAtlasCache ()
{
    Stop ();
}

bool SceneTextAtlasCache::Start (const uint8_t* fontBytes, size_t fontByteCount, std::string& error)
{
    Stop ();
    if (fontBytes == nullptr || fontByteCount == 0) {
        error = "the scene-text atlas cache font is empty";
        return false;
    }
    auto font = std::make_shared<const std::vector<uint8_t>> (fontBytes, fontBytes + fontByteCount);
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        impl_->stopping = false;
        impl_->running = true;
        impl_->queue.clear ();
        impl_->ready.clear ();
        impl_->active.clear ();
        impl_->failedGlyphs.clear ();
        impl_->pendingGlyphs.clear ();
        impl_->stagingBytes = 0;
        impl_->hits = impl_->misses = impl_->rejected = impl_->failures = 0;
        impl_->generatedPages = impl_->generatedGlyphs = impl_->generationMicroseconds = 0;
    }
    impl_->worker = std::thread ([this, font = std::move (font)] { impl_->Run (font); });
    return true;
}

void SceneTextAtlasCache::Stop ()
{
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        impl_->stopping = true;
        impl_->running = false;
        impl_->queue.clear ();
        impl_->ready.clear ();
        impl_->active.clear ();
        impl_->failedGlyphs.clear ();
        impl_->pendingGlyphs.clear ();
        impl_->stagingBytes = 0;
    }
    impl_->wake.notify_all ();
    if (impl_->worker.joinable ())
        impl_->worker.join ();
}

bool SceneTextAtlasCache::Request (const std::vector<uint32_t>& glyphIds)
{
    GlyphKey key = Normalize (glyphIds);
    std::lock_guard<std::mutex> lock (impl_->mutex);
    if (!impl_->running || impl_->stopping || key.empty () || key.size () > SceneTextAtlasPage::kMaximumGlyphs) {
        ++impl_->rejected;
        return false;
    }
    key.erase (std::remove_if (key.begin (), key.end (), [this] (uint32_t glyphId) {
                   return impl_->pendingGlyphs.find (glyphId) != impl_->pendingGlyphs.end () ||
                          impl_->failedGlyphs.find (glyphId) != impl_->failedGlyphs.end ();
               }),
               key.end ());
    if (key.empty ()) {
        ++impl_->hits;
        return true;
    }
    if (impl_->pendingGlyphs.size () + key.size () > kMaximumPendingGlyphs) {
        ++impl_->rejected;
        return false;
    }
    for (uint32_t glyphId : key)
        ++impl_->pendingGlyphs[glyphId];
    impl_->active.insert (key);
    impl_->queue.push_back (std::move (key));
    ++impl_->misses;
    impl_->wake.notify_one ();
    return true;
}

std::shared_ptr<const SceneTextAtlasPage> SceneTextAtlasCache::TakeReady ()
{
    std::lock_guard<std::mutex> lock (impl_->mutex);
    if (impl_->ready.empty ())
        return nullptr;
    ReadyPage result = std::move (impl_->ready.front ());
    impl_->ready.pop_front ();
    impl_->stagingBytes -= result.page->Pixels ().size ();
    impl_->RemovePending (result.key);
    impl_->active.erase (result.key);
    impl_->wake.notify_one ();
    return std::move (result.page);
}

SceneTextAtlasCacheStats SceneTextAtlasCache::Stats () const
{
    std::lock_guard<std::mutex> lock (impl_->mutex);
    SceneTextAtlasCacheStats stats;
    stats.running = impl_->running;
    stats.pending = impl_->pendingGlyphs.size ();
    stats.stagingPages = impl_->ready.size ();
    stats.stagingBytes = impl_->stagingBytes;
    stats.hits = impl_->hits;
    stats.misses = impl_->misses;
    stats.rejected = impl_->rejected;
    stats.failures = impl_->failures;
    stats.generatedPages = impl_->generatedPages;
    stats.generatedGlyphs = impl_->generatedGlyphs;
    stats.generationMicroseconds = impl_->generationMicroseconds;
    return stats;
}

} // namespace geomsrv::archviz
