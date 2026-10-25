#ifndef EVP_EDIT_EDITPATHOUTLINECACHE_HPP
#define EVP_EDIT_EDITPATHOUTLINECACHE_HPP

// A caller-owned cache of path outlines (edit::Outline), shared by the calls of one editing
// frame and the frames after it: snapping, paving and the layers all tessellate the same paths
// at the same steps, and between two frames of a drag only the dragged path changes.
//
// An entry is keyed by the path's id and step AND reused only while the path's own value is
// unchanged (compared on every lookup), so a stale outline can never outlive its input,
// whatever the document revision says. It touches no global, takes no lock and is for one
// thread: one per editor session, like PickGeometryCache. Results with and without it are
// identical (the replay's determinism gate).
#include "Edit/EditPath.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace geomsrv::edit {

class PathOutlineCache {
  public:
    // The outline of `path` sampled at most `maxStep` apart, built once per path value and
    // step; null when the path does not build (Outline's error is not kept).
    const PathOutline* Get (const Path& path, double maxStep);
    // Ends a frame: entries no lookup used since the previous Prune are dropped, so the cache
    // holds what the current document still asks for and nothing a deleted path left.
    void Prune ();
    size_t Size () const;

    uint64_t builds = 0; // outlines actually tessellated: the misses
    uint64_t hits = 0;

  private:
    struct Entry {
        Path input;
        double step = 0.0;
        bool built = false;
        PathOutline outline;
        uint64_t used = 0;
    };
    std::unordered_map<Id, std::vector<Entry>> entries_;
    uint64_t frame_ = 1;
};

} // namespace geomsrv::edit
#endif
