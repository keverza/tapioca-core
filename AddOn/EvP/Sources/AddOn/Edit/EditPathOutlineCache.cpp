#include "Edit/EditPathOutlineCache.hpp"
#include <algorithm>
#include <string>

namespace geomsrv::edit {

namespace {
constexpr size_t kStepsPerPath = 4; // the layers', the snap's, the preview's, and one spare
}

const PathOutline* PathOutlineCache::Get (const Path& path, double maxStep)
{
    auto& list = entries_[path.id];
    for (auto& e : list)
        if (e.step == maxStep && e.input == path) {
            e.used = frame_;
            ++hits;
            return e.built ? &e.outline : nullptr;
        }
    // A path that changed replaces its own entry at this step rather than piling up beside it.
    auto slot = std::find_if (list.begin (), list.end (), [&] (const Entry& e) { return e.step == maxStep; });
    if (slot == list.end ()) {
        if (list.size () >= kStepsPerPath)
            slot = std::min_element (list.begin (), list.end (),
                                     [] (const Entry& a, const Entry& b) { return a.used < b.used; });
        else
            slot = list.insert (list.end (), Entry {});
    }
    Entry& e = *slot;
    e.input = path;
    e.step = maxStep;
    e.used = frame_;
    std::string error;
    e.built = Outline (path, maxStep, e.outline, error);
    ++builds;
    return e.built ? &e.outline : nullptr;
}

void PathOutlineCache::Prune ()
{
    for (auto it = entries_.begin (); it != entries_.end ();) {
        auto& list = it->second;
        list.erase (std::remove_if (list.begin (), list.end (), [&] (const Entry& e) { return e.used < frame_; }),
                    list.end ());
        it = list.empty () ? entries_.erase (it) : std::next (it);
    }
    ++frame_;
}

size_t PathOutlineCache::Size () const
{
    size_t n = 0;
    for (const auto& [id, list] : entries_)
        n += list.size ();
    return n;
}

} // namespace geomsrv::edit
