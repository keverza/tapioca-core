// ArchViz/SlabBodies -- see the header.

#include "ArchViz/SlabBodies.hpp"

#include <iterator>
#include <mutex>

namespace geomsrv {
namespace archviz {
namespace slabbodies {

namespace {

std::mutex g_mutex;
std::set<std::string> g_wanted;
std::shared_ptr<const Bodies> g_latest;

} // namespace

void Want (const std::vector<std::string>& guids)
{
    std::lock_guard<std::mutex> lock (g_mutex);
    g_wanted = std::set<std::string> (guids.begin (), guids.end ());
}

std::set<std::string> Wanted ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    return g_wanted;
}

void Publish (std::vector<Mesh> meshes)
{
    std::lock_guard<std::mutex> lock (g_mutex);
    auto next = std::make_shared<Bodies> ();
    if (g_latest != nullptr)
        *next = *g_latest;
    ++next->generation;
    for (Mesh& mesh : meshes) {
        // Only what is still wanted: the slices may have let a slab go while the pass ran.
        if (g_wanted.count (mesh.guid) != 0)
            next->meshes[mesh.guid] = std::move (mesh);
    }
    for (auto held = next->meshes.begin (); held != next->meshes.end ();)
        held = g_wanted.count (held->first) != 0 ? std::next (held) : next->meshes.erase (held);
    g_latest = std::move (next);
}

std::shared_ptr<const Bodies> Latest ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    return g_latest;
}

void Clear ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    g_wanted.clear ();
    g_latest.reset ();
}

} // namespace slabbodies
} // namespace archviz
} // namespace geomsrv
