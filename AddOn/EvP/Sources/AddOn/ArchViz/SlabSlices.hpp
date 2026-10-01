#ifndef EVP_ARCHVIZ_SLABSLICES_HPP
#define EVP_ARCHVIZ_SLABSLICES_HPP

// ArchViz/SlabSlices -- a massing slab's floors as slices: the heights its floors
// start at under a rule, and the slab's outline at each of them, holes and arcs
// included, with the slice's area.
//
// A massing slab is one building block modelled as a thick slab: its outline is the
// footprint and its thickness the building's height. The floors are cut from its
// bottom up, the way the feasibility figures count them:
//
//   storeys       the project's storey heights, from the slab's bottom: the first
//                 floor is as high as the storey the bottom sits in, the next as the
//                 storey above it, and past the top storey the last height repeats
//   storeyLevels  at the project's storey levels that lie within the slab
//   step          a fixed floor height from the slab's bottom
//   levels        at the heights given, world metres
//
// In every rule but `levels`, a floor cut short by the slab's top counts only when it
// is at least `minTopMetres` high (2.5 m): a complete storey is a floor however low
// it is, a sliver under the roof is not.
//
// ⚠️ THE SLAB IS A PRISM HERE. Its outline is its polygon at every height, which is
// what a massing slab is and what the feasibility figures assume; an edge trimmed off
// vertical is COUNTED (`slopedEdges`), and its cut is drawn as if it were vertical.
//
// Pure -- no DevKit -- so tests/cpp builds it: a floor too many is a wrong gross area,
// and nothing in the picture says so. The slabs are read by SlabSliceSource.hpp.

#include "ArchViz/ExtractionStorySlices.hpp" // ProjectStoreys
#include "ArchViz/StorySliceOverlayContent.hpp"
#include "Geometry/Mesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace slabslices {

// One contour of the slab's polygon: x, y metres with no closing repeat, and one signed
// arc angle per vertex for the edge leaving it (0 or absent: straight), as Archicad's
// polygon memo carries them.
struct Ring {
    std::vector<double> xy;
    std::vector<double> arcs;
};

struct Slab {
    std::string guid;
    std::string id;      // the Element Settings ID; may be empty
    double bottom = 0.0; // world Z metres
    double top = 0.0;
    Ring outer;
    std::vector<Ring> holes;
    uint32_t slopedEdges = 0; // edges trimmed off vertical: cut here as if vertical
};

enum class Cut { Storeys, StoreyLevels, Step, Levels };

struct Rule {
    Cut cut = Cut::Storeys;
    double stepMetres = 3.0;    // Cut::Step
    std::vector<double> levels; // Cut::Levels, world Z metres
    double offsetMetres = 0.0;  // each cut this far above its floor's base, kept inside the slab
    double minTopMetres = 2.5;  // a floor cut short by the top counts from this height (not for Levels)
};

// "storeys", "storeyLevels", "step", "levels".
const char* CutName (Cut cut);

struct Floor {
    double base = 0.0;   // world Z metres
    double height = 0.0; // to the next floor's base, or to the slab's top
    double areaM2 = 0.0; // its slice; 0 where a body has no cross-section at its cut
};

// The floors of a slab from `bottom` to `top` under `rule`, bottom first. `levels` are
// the project's storey levels, rising. Empty with `problem` set when the rule gives none.
std::vector<Floor> Floors (double bottom, double top, const Rule& rule, const std::vector<double>& levels,
                           std::string& problem);

// A ring as a closed contour, arcs in chords of at most `chordMetres`.
SliceChain Contour (const Ring& ring, double chordMetres);

// The storey a height lies in: the highest storey whose level is at or below it, or
// the lowest storey below them all; 0 with no storeys.
int StoreyAt (const ProjectStoreys& storeys, double z);

// ⚠️ THE STOREY OF EACH FLOOR, `heights` RISING (the user, 2026-10-01: "do not reuse the
// previous number, increment as it is a new storey"). Within the project's storeys, the one
// it lies in (`StoreyAt`); a floor above the top storey's level is a storey of its own,
// numbered on from the top one, and a floor below the lowest is numbered down from it -- a
// storey with no name never repeats another's number. All 0 with no storeys.
std::vector<int> StoreysAt (const ProjectStoreys& storeys, const std::vector<double>& heights);

struct Summary {
    std::string guid;
    std::string id;
    double bottom = 0.0;
    double top = 0.0;
    double footprintM2 = 0.0; // the outer contour alone: the feasibility figures' footprint
    double sliceAreaM2 = 0.0; // one slice, its holes subtracted
    double areaM2 = 0.0;      // every slice
    std::vector<Floor> floors;
    uint32_t slopedEdges = 0;
    std::string problem; // why there are no floors, when there are none
    // Cut from the slab's 3D body (`SliceBody`), not its polygon; and its solid element
    // operations' operators, which the caller counts.
    bool body = false;
    uint32_t operators = 0;
};

// The slices of one slab, appended to `out`: one per floor, at its base plus the
// rule's offset, named "<ID> F<n>" ("Slab F<n>" without an ID).
Summary SliceSlab (const Slab& slab, const Rule& rule, const ProjectStoreys& storeys,
                   std::vector<storysliceoverlay::Slice>& out);

// ⚠️ A SLAB SOLID ELEMENT OPERATIONS CUT IS NOT ITS POLYGON. A subtraction lives only in
// the 3D model: the slab's record, and its 3D conversion through ModelAccess, are its
// own geometry from before the operation (HANDOFF-SolidOperations §4). `body` is the
// mesh the extraction read from the 3D model -- the one the overlays' wireframe draws,
// operations applied. Same floors, cut heights and names as `SliceSlab`; each slice is
// the body's cross-section at its cut, its rings even-odd (a subtracted courtyard is a
// hole, not filled by a union), and a floor the operations emptied has no slice. The
// footprint stays the polygon's: what the feasibility figures call it; `sliceAreaM2` is
// the largest slice.
Summary SliceBody (const Slab& slab, const Mesh& body, const Rule& rule, const ProjectStoreys& storeys,
                   std::vector<storysliceoverlay::Slice>& out);

} // namespace slabslices
} // namespace archviz
} // namespace geomsrv

#endif
