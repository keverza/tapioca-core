#include "Edit/EditPathOutlineCache.hpp"
#include <algorithm>
#include <string>

namespace geomsrv::edit {

namespace {
constexpr size_t kStepsPerPath = 4; // the layers', the snap's, the preview's, and one spare
}

const PathOutline* PathOutlineCache::Get (const Path& path, double maxStep)
{
    const PathPreparation* preparation = nullptr;
    return Get (path, maxStep, preparation);
}

const PathOutline* PathOutlineCache::Get (const Path& path, double maxStep, const PathPreparation*& preparation)
{
    preparation = nullptr;
    auto& list = entries_[path.id];
    for (auto& e : list)
        if (e.step == maxStep && e.input == path) {
            e.used = frame_;
            ++hits;
            preparation = &e.preparation;
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
    AlignmentGeometry geometry;
    e.built = PreparePath (path, geometry, e.preparation, error) &&
              Outline (path, geometry, e.preparation, maxStep, e.outline, error);
    ++builds;
    preparation = &e.preparation;
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

void PathOutlineCache::Retain (const std::vector<Path>& paths)
{
    for (auto it = entries_.begin (); it != entries_.end ();)
        it = std::any_of (paths.begin (), paths.end (), [&] (const Path& p) { return p.id == it->first; })
                 ? std::next (it)
                 : entries_.erase (it);
}

void PathOutlineCache::Adopt (PathOutlineCache&& other)
{
    for (auto& [id, theirs] : other.entries_) {
        auto& list = entries_[id];
        for (auto& e : theirs) {
            auto same = std::find_if (list.begin (), list.end (), [&] (const Entry& m) { return m.step == e.step; });
            if (same != list.end () && same->input == e.input)
                continue; // already here: keep ours
            e.used = frame_;
            if (same != list.end ())
                *same = std::move (e); // ours is of an older value of the path
            else if (list.size () < kStepsPerPath)
                list.push_back (std::move (e));
        }
    }
}

bool PathOutlineCache::Holds (Id id) const
{
    return entries_.count (id) != 0;
}

size_t PathOutlineCache::Size () const
{
    size_t n = 0;
    for (const auto& [id, list] : entries_)
        n += list.size ();
    return n;
}

} // namespace geomsrv::edit
