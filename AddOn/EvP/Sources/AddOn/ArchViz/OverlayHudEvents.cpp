// ArchViz/OverlayHudEvents -- see the header.

#include "ArchViz/OverlayHudEvents.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

namespace geomsrv {
namespace archviz {
namespace overlayhudevents {

namespace {

std::mutex g_mutex;
std::deque<Event> g_ring;
uint64_t g_last = 0;

} // namespace

uint64_t Push (Event event)
{
    const auto now = std::chrono::system_clock::now ().time_since_epoch ();
    std::lock_guard<std::mutex> lock (g_mutex);
    event.seq = ++g_last;
    event.timeMs = uint64_t (std::chrono::duration_cast<std::chrono::milliseconds> (now).count ());
    g_ring.push_back (std::move (event));
    while (g_ring.size () > kCapacity)
        g_ring.pop_front ();
    return g_last;
}

Tail Since (uint64_t since, size_t max)
{
    std::lock_guard<std::mutex> lock (g_mutex);
    Tail tail;
    tail.lastSeq = g_last;
    // The oldest still held; the one after `since` must be among them, or it is lost.
    const uint64_t oldest = g_ring.empty () ? g_last + 1 : g_ring.front ().seq;
    tail.gap = since + 1 < oldest && since < g_last;
    for (const Event& event : g_ring) {
        if (event.seq <= since)
            continue;
        if (tail.events.size () >= (std::max) (max, size_t (1)))
            break;
        tail.events.push_back (event);
    }
    return tail;
}

uint64_t LastSeq ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    return g_last;
}

void Clear ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    g_ring.clear ();
}

} // namespace overlayhudevents
} // namespace archviz
} // namespace geomsrv
