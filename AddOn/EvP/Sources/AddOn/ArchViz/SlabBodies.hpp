#ifndef EVP_ARCHVIZ_SLABBODIES_HPP
#define EVP_ARCHVIZ_SLABBODIES_HPP

// ArchViz/SlabBodies -- the 3D bodies of the slabs the storey slices cut from their body
// (SlabSlices `SliceBody`): which slabs are wanted, and the latest bodies the extraction
// read for them.
//
// ⚠️ FROM THE PASS THAT DRAWS THE WIREFRAME, AFTER IT. A solid element operation exists
// only in the 3D model, and the pass reads the 3D model as the modeler has it -- the
// wireframe drawn from it showed the user's subtraction while the slices did not (the
// user, 2026-10-01). The pass hands over the wanted slabs' meshes as it walks them
// (StorySliceAccumulator) and publishes them only when it finished: a pass stopped half
// way may not have reached a slab, and a slab it did not reach keeps its last body.
//
// THREADS. `Want`, `Latest` and `Clear` on the main thread; `Wanted` and `Publish` on the
// extraction worker, between gate slices. One mutex; nothing here on a render thread.

#include "Geometry/Mesh.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace slabbodies {

// The slabs whose bodies the next pass should hand over; empty wants none.
void Want (const std::vector<std::string>& guids);
// What a pass reads at its start.
std::set<std::string> Wanted ();

struct Bodies {
    uint64_t generation = 0; // one per publish; 0 before any
    std::map<std::string, Mesh> meshes;
};

// A finished pass's bodies of the wanted slabs, merged over the last.
void Publish (std::vector<Mesh> meshes);
std::shared_ptr<const Bodies> Latest ();

// The slices are off, or the project closed (§8): nothing wanted, nothing held.
void Clear ();

} // namespace slabbodies
} // namespace archviz
} // namespace geomsrv

#endif
