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
std::map<std::string, std::set<std::string>> s_owners;
std::map<std::string, uint64_t> s_tickets;
uint64_t s_serial = 0;
std::shared_ptr<const Bodies> g_latest;

} // namespace

void Want (const std::vector<std::string>& guids, const std::string& owner)
{
    std::lock_guard<std::mutex> lock (g_mutex);
    const std::set<std::string> wanted (guids.begin (), guids.end ());
    if (s_owners.count (owner) && s_owners.at (owner) == wanted)
        return;
    s_owners[owner] = wanted;
    g_wanted.clear ();
    for (const auto& entry : s_owners)
        g_wanted.insert (entry.second.begin (), entry.second.end ());
    for (const auto& guid : g_wanted)
        if (!s_tickets.count (guid))
            s_tickets[guid] = ++s_serial;
    for (auto it = s_tickets.begin (); it != s_tickets.end ();)
        it = g_wanted.count (it->first) ? std::next (it) : s_tickets.erase (it);
    if (g_latest) {
        auto next = std::make_shared<Bodies> (*g_latest);
        for (auto it = next->meshes.begin (); it != next->meshes.end ();)
            it = g_wanted.count (it->first) ? std::next (it) : next->meshes.erase (it);
        g_latest = std::move (next);
    }
}

void Invalidate (const std::vector<std::string>& guids)
{
    std::lock_guard<std::mutex> lock (g_mutex);
    auto next = g_latest ? std::make_shared<Bodies> (*g_latest) : std::make_shared<Bodies> ();
    ++next->generation;
    for (const auto& guid : guids) {
        if (g_wanted.count (guid))
            s_tickets[guid] = ++s_serial;
        next->meshes.erase (guid);
    }
    g_latest = std::move (next);
}

std::map<std::string, uint64_t> Capture ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    return s_tickets;
}

std::set<std::string> Wanted ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    return g_wanted;
}

void Publish (std::vector<Mesh> meshes)
{
    Publish (std::move (meshes), Capture ());
}

void Publish (std::vector<Mesh> meshes, const std::map<std::string, uint64_t>& captured)
{
    std::lock_guard<std::mutex> lock (g_mutex);
    auto next = std::make_shared<Bodies> ();
    if (g_latest != nullptr)
        *next = *g_latest;
    ++next->generation;
    // A completed pass that did not encounter a requested slab must not retain
    // yesterday's body (hidden/deleted elements have no current extractable body).
    for (const auto& entry : captured)
        if (s_tickets.count (entry.first) && s_tickets.at (entry.first) == entry.second)
            next->meshes.erase (entry.first);
    for (Mesh& mesh : meshes) {
        // Only what is still wanted: the slices may have let a slab go while the pass ran.
        if (captured.count (mesh.guid) && s_tickets.count (mesh.guid) &&
            captured.at (mesh.guid) == s_tickets.at (mesh.guid))
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
    s_owners.clear ();
    s_tickets.clear ();
    g_latest.reset ();
}

} // namespace slabbodies
} // namespace archviz
} // namespace geomsrv
