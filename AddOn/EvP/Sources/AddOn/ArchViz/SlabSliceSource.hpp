#ifndef EVP_ARCHVIZ_SLABSLICESOURCE_HPP
#define EVP_ARCHVIZ_SLABSLICESOURCE_HPP

// ArchViz/SlabSliceSource -- the massing slabs, read from Archicad for their slices
// (SlabSlices.hpp): the selection as element GUIDs, each slab's polygon with its holes
// and arcs, its bottom and top in world Z, and each element's modification stamp, which
// is how the storey slices follow an edit.
//
// ⚠️ FROM THE DATABASE, NOT THE 3D MODEL. A slab's record is current the moment an edit
// ends; the 3D window's model is rebuilt on Archicad's own schedule, so a cut of it
// taken on the edit's tick would draw the slab as it was.
//
// ⚠️ THE HEIGHTS: top = home storey's level + `level` + `offsetFromTop`, bottom = top -
// `thickness`. `level` is the reference plane's height above the home storey and
// `offsetFromTop` the reference plane's distance below the slab's top (0 for a slab
// referenced at its top, its thickness for one referenced at its bottom), so this holds
// for every reference plane (APIdefs_Elements.h, API_SlabType).
//
// MAIN THREAD ONLY: all of it is ACAPI.

#include "ArchViz/SlabSlices.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace slabsource {

// The elements selected in Archicad now, as GUID strings. False with `error` on a
// failed read; nothing selected is true and empty.
bool Selected (std::vector<std::string>& guids, std::string& error);

struct Skip {
    std::string guid;
    std::string reason; // "not found", "Wall - not a slab", ...
};

struct Reading {
    std::vector<slabslices::Slab> slabs;
    std::vector<Skip> skipped;
};

// Each element of `guids` read as a slab; anything else skipped with its reason.
Reading Read (const std::vector<std::string>& guids, const ProjectStoreys& storeys);

// Each element's modification stamp, 0 for one that no longer exists.
std::vector<uint64_t> Stamps (const std::vector<std::string>& guids);

} // namespace slabsource
} // namespace archviz
} // namespace geomsrv

#endif
