#ifndef GEOMSRV_ARCHVIZ_MODELSTAMPWALK_HPP
#define GEOMSRV_ARCHVIZ_MODELSTAMPWALK_HPP

// ArchViz/ModelStampWalk -- the model watch's cheap change detector (#97).
//
// Bound by OVERLAY-INVARIANTS.md as part of ArchViz/ModelWatch: read section 5
// (the watch POLLS, it never attaches an observer -- PLAT-RE68) before editing.
//
// ⚠️ WHY NOT ARCHICAD'S 3D DIFFERENCE GENERATOR ON EVERY EDIT. It holds the main
// thread ~3.1 s on an 806k-triangle model, and the user saw Archicad stop
// responding around every update (2026-10-31). This walk lists the elements the
// 3D window shows (ACAPI_Element_GetElemList, APIFilt_In3D) and compares each
// header's modiStamp with the last walk: reads only, nothing written.
//
// ⚠️ WHAT AN ELEMENT'S OWN STAMP DOES NOT SAY. An element whose 3D changes only
// through another -- a wall trimmed by a moved slab, a host cut by a moved
// window, a solid-element-operation target -- keeps its stamp. So every change
// also re-extracts the published elements whose boxes touch the changed one's
// old and new boxes (Neighbours), and the 3D generator still runs as a BACKSTOP
// (Backstop3DDue) once the user has been idle, for what neither reaches: a
// surface material edit, a neighbour past the cap.

#include "Geometry/Mesh.hpp"    // Snapshot, Mesh
#include "Notify/ModelDiff.hpp" // modeldiff::Result

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace stampwalk {

using Stamps = std::map<std::string, uint64_t>; // element GUID -> header modiStamp

struct Box {
    double lo[3] = { 0, 0, 0 };
    double hi[3] = { 0, 0, 0 };
};

constexpr double kNeighbourToleranceM = 0.05;  // boxes this close are touching
constexpr size_t kNeighbourCap = 64;           // past this the backstop catches the rest
constexpr int64_t kBackstopIdleMs = 20000;     // no stamp move at all for this long ...
constexpr int64_t kBackstopSpacingMs = 120000; // ... and at least this since the last one

// What changed between two walks -- pure.
inline void Compare (const Stamps& before, const Stamps& after, modeldiff::Result& diff)
{
    for (const auto& [guid, stamp] : after) {
        const auto it = before.find (guid);
        if (it == before.end ())
            diff.created.push_back (guid);
        else if (it->second != stamp)
            diff.modified.push_back (guid);
    }
    for (const auto& entry : before)
        if (after.count (entry.first) == 0)
            diff.deleted.push_back (entry.first);
}

inline bool Touches (const Box& a, const Box& b, double tolerance)
{
    for (int k = 0; k < 3; ++k)
        if (a.lo[k] > b.hi[k] + tolerance || b.lo[k] > a.hi[k] + tolerance)
            return false;
    return true;
}

// The candidates touching any changed box, not already named, at most `cap` --
// pure. `dropped` counts the ones past the cap.
inline std::vector<std::string> Neighbours (const std::vector<Box>& changed,
                                            const std::vector<std::pair<std::string, Box>>& candidates,
                                            const std::set<std::string>& named, double tolerance, size_t cap,
                                            size_t& dropped)
{
    std::vector<std::string> out;
    dropped = 0;
    for (const auto& [guid, box] : candidates) {
        if (named.count (guid) != 0)
            continue;
        const bool touches =
            std::any_of (changed.begin (), changed.end (), [&] (const Box& c) { return Touches (c, box, tolerance); });
        if (!touches)
            continue;
        if (out.size () < cap)
            out.push_back (guid);
        else
            ++dropped;
    }
    return out;
}

// Each published mesh's box -- pure. A mesh with no vertices has none.
inline std::vector<std::pair<std::string, Box>> MeshBoxes (const Snapshot& snapshot)
{
    std::vector<std::pair<std::string, Box>> boxes;
    boxes.reserve (snapshot.meshes.size ());
    for (const Mesh& mesh : snapshot.meshes) {
        if (mesh.vertices.size () < 3)
            continue;
        Box box;
        for (int k = 0; k < 3; ++k)
            box.lo[k] = box.hi[k] = mesh.vertices[k];
        for (size_t v = 3; v + 2 < mesh.vertices.size (); v += 3)
            for (int k = 0; k < 3; ++k) {
                box.lo[k] = (std::min) (box.lo[k], mesh.vertices[v + k]);
                box.hi[k] = (std::max) (box.hi[k], mesh.vertices[v + k]);
            }
        boxes.emplace_back (mesh.guid, box);
    }
    return boxes;
}

// Whether the 3D generator runs as the backstop on this tick -- pure. Only after
// something moved since it last ran, only once the project has been still for
// kBackstopIdleMs (its ~3 s hold lands while the user is not working), and never
// sooner than kBackstopSpacingMs after the previous one.
inline bool Backstop3DDue (bool movedSince3D, int64_t sinceLastStampMoveMs, int64_t sinceLast3DMs)
{
    return movedSince3D && sinceLastStampMoveMs >= kBackstopIdleMs && sinceLast3DMs >= kBackstopSpacingMs;
}

// MAIN THREAD. The walk itself, with the previous walk as its baseline. The
// first poll (or `reset`) is the baseline: `firstCall`, nothing reported.
class Walker final {
  public:
    modeldiff::Result Poll (bool reset = false);
    size_t Count () const
    {
        return stamps_.size ();
    }
    void Clear ()
    {
        stamps_.clear ();
        have_ = false;
    }

  private:
    Stamps stamps_;
    bool have_ = false;
};

// MAIN THREAD. The element's current 3D box (ACAPI_Element_CalcBounds).
bool ElementBox (const std::string& guid, Box& box);

} // namespace stampwalk
} // namespace archviz
} // namespace geomsrv

#endif
