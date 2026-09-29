// ArchViz/StorySliceSnapshot -- see the header.

#include "ArchViz/StorySliceSnapshot.hpp"

#include <mutex>

namespace geomsrv {
namespace archviz {
namespace storeyslices {

namespace {

std::mutex g_mutex;
std::shared_ptr<const Snapshot> g_latest;
uint64_t g_generation = 0;

} // namespace

void Publish (std::vector<Storey> storeys)
{
    auto snapshot = std::make_shared<Snapshot> ();
    snapshot->storeys = std::move (storeys);
    const std::lock_guard<std::mutex> lock (g_mutex);
    snapshot->generation = ++g_generation;
    g_latest = std::move (snapshot);
}

std::shared_ptr<const Snapshot> Latest ()
{
    const std::lock_guard<std::mutex> lock (g_mutex);
    return g_latest;
}

void Clear ()
{
    const std::lock_guard<std::mutex> lock (g_mutex);
    g_latest.reset ();
    ++g_generation;
}

} // namespace storeyslices
} // namespace archviz
} // namespace geomsrv
