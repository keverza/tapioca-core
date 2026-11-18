#ifndef EVP_ARCHVIZ_SLABBODIES_HPP
#define EVP_ARCHVIZ_SLABBODIES_HPP

// ArchViz/SlabBodies -- the 3D bodies of the slabs the storey slices cut from their body
// (SlabSlices `SliceBody`): which slabs are wanted, and the latest bodies the extraction
// read for them.
// The legacy cache name is retained; GUID capture is type-neutral and also
// supplies defined Mesh/Morph massing sources with their operated bodies.
//
// ⚠️ FROM THE PASS THAT DRAWS THE WIREFRAME, AFTER IT. A solid element operation exists
// only in the 3D model, and the pass reads the 3D model as the modeler has it -- the
// wireframe drawn from it showed the user's subtraction while the slices did not (the
// user, 2026-10-01). The pass hands over the wanted slabs' meshes as it walks them
// (StorySliceAccumulator) and publishes them only when it finished: a pass stopped half
// way may not have reached a slab and publishes nothing. A completed full pass that
// did not encounter a requested slab removes its previous body.
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

// Each consumer's wanted slabs; empty releases only that consumer. Extraction
// captures their union, so standalone and feasibility cannot cancel one another.
void Want (const std::vector<std::string>& guids, const std::string& owner = "storySlices");
// Invalidate edited sources before asking for another pass. Tickets reject a pass
// that started before the edit, even if it completes afterwards.
void Invalidate (const std::vector<std::string>& guids);
// The model moved but these sources' own records did not (an SEO operator or a link may
// have changed them, or nothing has): a pass that started before now is rejected as by
// Invalidate, but the held bodies stay published until the next pass replaces them.
// Erasing them would publish an "awaiting a body" result between two identical ones (#235).
void Revalidate (const std::vector<std::string>& guids);
std::map<std::string, uint64_t> Capture ();
// What a pass reads at its start.
std::set<std::string> Wanted ();

struct Bodies {
    uint64_t generation = 0; // one per publish; 0 before any
    std::map<std::string, Mesh> meshes;
};

// A finished full pass's bodies, accepted only for still-current capture tickets.
void Publish (std::vector<Mesh> meshes);
void Publish (std::vector<Mesh> meshes, const std::map<std::string, uint64_t>& captured);
std::shared_ptr<const Bodies> Latest ();

// True when `a` and `b` hold the same set of `guids` with identical vertices and
// triangles -- what the slices read. A newer generation with the same geometry is then
// no reason to rebuild. Either null counts as different unless `guids` is empty.
bool SameGeometry (const Bodies* a, const Bodies* b, const std::vector<std::string>& guids);

// Project closed (§8): release every consumer and every cached body.
void Clear ();

} // namespace slabbodies
} // namespace archviz
} // namespace geomsrv

#endif
