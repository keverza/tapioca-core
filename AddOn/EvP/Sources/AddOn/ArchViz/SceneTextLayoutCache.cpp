#include "ArchViz/SceneTextLayoutCache.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace geomsrv::archviz {
namespace {

constexpr size_t kMaximumTextBytes = 4096;

struct LayoutKey {
    std::string utf8;
    SceneTextDirection direction = SceneTextDirection::Auto;

    bool operator< (const LayoutKey& other) const
    {
        if (utf8 != other.utf8)
            return utf8 < other.utf8;
        return direction < other.direction;
    }
};

enum class EntryState : uint8_t { Pending, Ready, Failed };

struct LayoutEntry {
    EntryState state = EntryState::Pending;
    std::shared_ptr<const SceneTextGlyphRun> run;
    std::string error;
    uint64_t lastAccess = 0;
};

} // namespace

struct SceneTextLayoutCache::Impl {
    explicit Impl (size_t requestedCapacity) : capacity ((std::max<size_t>) (requestedCapacity, 1))
    {
    }

    const size_t capacity;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable startup;
    std::condition_variable completed;
    std::map<LayoutKey, LayoutEntry> entries;
    std::deque<LayoutKey> queue;
    std::thread worker;
    bool running = false;
    bool stopping = false;
    bool startupComplete = false;
    bool startupSucceeded = false;
    std::string startupError;
    uint64_t accessSequence = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t rejected = 0;
    uint64_t failures = 0;

    void Run (std::vector<uint8_t> fontBytes)
    {
        SceneTextShaper shaper;
        std::string error;
        const bool initialized = shaper.Init (fontBytes.data (), fontBytes.size (), error);
        {
            std::lock_guard<std::mutex> lock (mutex);
            startupComplete = true;
            startupSucceeded = initialized;
            startupError = error;
            running = initialized;
        }
        startup.notify_all ();
        if (!initialized)
            return;

        for (;;) {
            LayoutKey key;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait (lock, [this] { return stopping || !queue.empty (); });
                if (stopping)
                    break;
                key = std::move (queue.front ());
                queue.pop_front ();
            }

            auto run = std::make_shared<SceneTextGlyphRun> ();
            error.clear ();
            const bool shaped = shaper.Shape (key.utf8, key.direction, *run, error);
            {
                std::lock_guard<std::mutex> lock (mutex);
                auto found = entries.find (key);
                if (found != entries.end () && found->second.state == EntryState::Pending) {
                    found->second.state = shaped ? EntryState::Ready : EntryState::Failed;
                    found->second.run = shaped ? std::shared_ptr<const SceneTextGlyphRun> (std::move (run)) : nullptr;
                    found->second.error = shaped ? std::string {} : error;
                    if (!shaped)
                        ++failures;
                }
            }
            completed.notify_all ();
        }
    }

    bool EvictOne ()
    {
        auto victim = entries.end ();
        for (auto it = entries.begin (); it != entries.end (); ++it) {
            if (it->second.state == EntryState::Pending)
                continue;
            if (victim == entries.end () || it->second.lastAccess < victim->second.lastAccess)
                victim = it;
        }
        if (victim == entries.end ())
            return false;
        entries.erase (victim);
        return true;
    }
};

SceneTextLayoutCache::SceneTextLayoutCache (size_t capacity) : impl_ (new Impl (capacity))
{
}

SceneTextLayoutCache::~SceneTextLayoutCache ()
{
    Stop ();
}

bool SceneTextLayoutCache::Start (const uint8_t* fontBytes, size_t fontByteCount, std::string& error)
{
    Stop ();
    if (fontBytes == nullptr || fontByteCount == 0) {
        error = "the scene-text layout cache font is empty";
        return false;
    }
    std::vector<uint8_t> font (fontBytes, fontBytes + fontByteCount);
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        impl_->stopping = false;
        impl_->startupComplete = false;
        impl_->startupSucceeded = false;
        impl_->startupError.clear ();
        impl_->entries.clear ();
        impl_->queue.clear ();
        impl_->accessSequence = 0;
        impl_->hits = impl_->misses = impl_->rejected = impl_->failures = 0;
    }
    impl_->worker = std::thread ([this, font = std::move (font)] () mutable { impl_->Run (std::move (font)); });
    std::unique_lock<std::mutex> lock (impl_->mutex);
    impl_->startup.wait (lock, [this] { return impl_->startupComplete; });
    if (!impl_->startupSucceeded) {
        error = impl_->startupError;
        lock.unlock ();
        Stop ();
        return false;
    }
    return true;
}

void SceneTextLayoutCache::Stop ()
{
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        impl_->stopping = true;
        impl_->running = false;
        impl_->queue.clear ();
        for (auto it = impl_->entries.begin (); it != impl_->entries.end ();) {
            if (it->second.state == EntryState::Pending)
                it = impl_->entries.erase (it);
            else
                ++it;
        }
    }
    impl_->wake.notify_all ();
    impl_->completed.notify_all ();
    if (impl_->worker.joinable ())
        impl_->worker.join ();
}

std::shared_ptr<const SceneTextGlyphRun> SceneTextLayoutCache::FindOrRequest (const std::string& utf8,
                                                                              SceneTextDirection direction)
{
    if (utf8.empty () || utf8.size () > kMaximumTextBytes) {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        ++impl_->rejected;
        return nullptr;
    }
    const LayoutKey key { utf8, direction };
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        if (!impl_->running || impl_->stopping) {
            ++impl_->rejected;
            return nullptr;
        }
        auto found = impl_->entries.find (key);
        if (found != impl_->entries.end ()) {
            found->second.lastAccess = ++impl_->accessSequence;
            ++impl_->hits;
            return found->second.state == EntryState::Ready ? found->second.run : nullptr;
        }
        if (impl_->entries.size () >= impl_->capacity && !impl_->EvictOne ()) {
            ++impl_->rejected;
            return nullptr;
        }
        LayoutEntry entry;
        entry.lastAccess = ++impl_->accessSequence;
        impl_->entries.emplace (key, std::move (entry));
        impl_->queue.push_back (key);
        ++impl_->misses;
    }
    impl_->wake.notify_one ();
    return nullptr;
}

std::shared_ptr<const SceneTextGlyphRun>
SceneTextLayoutCache::WaitFor (const std::string& utf8, SceneTextDirection direction, std::string& error)
{
    if (auto ready = FindOrRequest (utf8, direction))
        return ready;
    const LayoutKey key { utf8, direction };
    std::unique_lock<std::mutex> lock (impl_->mutex);
    impl_->completed.wait (lock, [&] {
        const auto found = impl_->entries.find (key);
        return impl_->stopping || found == impl_->entries.end () || found->second.state != EntryState::Pending;
    });
    const auto found = impl_->entries.find (key);
    if (found != impl_->entries.end () && found->second.state == EntryState::Ready)
        return found->second.run;
    error = found != impl_->entries.end () ? found->second.error : "scene-text shaping was stopped or rejected";
    return nullptr;
}

SceneTextLayoutCacheStats SceneTextLayoutCache::Stats () const
{
    std::lock_guard<std::mutex> lock (impl_->mutex);
    SceneTextLayoutCacheStats stats;
    stats.running = impl_->running;
    stats.entries = impl_->entries.size ();
    stats.hits = impl_->hits;
    stats.misses = impl_->misses;
    stats.rejected = impl_->rejected;
    stats.failures = impl_->failures;
    for (const auto& [key, entry] : impl_->entries) {
        (void) key;
        stats.pending += entry.state == EntryState::Pending ? 1u : 0u;
    }
    return stats;
}

} // namespace geomsrv::archviz
