#ifndef EVP_ARCHVIZ_SECTIONMODEL_HPP
#define EVP_ARCHVIZ_SECTIONMODEL_HPP

// ArchViz/SectionModel -- the building section's model, read from Archicad for the HUD
// (HudSection.hpp): the selected elements' massing slabs, cut into floors at the project's
// storeys (SlabSliceSource.hpp, SlabSlices.hpp), each floor's slices and each slab's
// metadata; and the floors the user picked drawn on the 3D overlay as the add-on's own layer.
//
// ⚠️ THE SLAB'S POLYGON, NOT ITS BODY: a solid element operation's cut is in the 3D model only
// (SlabSlices.hpp `SliceBody`), and the section is read on a selection change, not after an
// extraction pass. The storey slices' layer is the one that follows bodies.
//
// MAIN THREAD: all of it is ACAPI, or the overlay's layer store.

#include "ArchViz/HudSection.hpp"
#include "ArchViz/StorySliceOverlayContent.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace sectionmodel {

// The picked floors' layer on the overlays: the add-on's own, never a caller's.
constexpr char kLayerName[] = "tapioca.floorPicks";
// The most slabs a section is read from: a selection of a whole district is not a building.
constexpr size_t kMostSlabs = 64;

struct Reading {
    hudsection::Section section;                  // not known: no massing slab among the elements
    std::vector<storysliceoverlay::Slice> slices; // every floor's, each with its storey
    std::vector<std::string> slabs;               // the slabs read, in order
};

// The section of the massing slabs among `guids`.
Reading Read (const std::vector<std::string>& guids);

// `run`'s floors of `slices` on the 3D overlay, outlined and filled in `rgba`; the layer gone
// when the run has none.
void Show (const std::vector<storysliceoverlay::Slice>& slices, const hudsection::Run& run, uint32_t rgba);
void Hide ();
// Whether the layer is in the store: the overlays' release clears every layer.
bool Shown ();

// ⚠️ ARCHICAD'S SELECTION'S SECTION FOR A HUD THAT CANNOT READ THE PROJECT -- the viewer's, on
// its render thread (the user: one HUD for the overlays and the viewer). What was last read;
// asked for before any read, it asks for one. Read again on the main thread when Archicad's
// selection changes while the viewer is open (`SelectionChanged`) and after the viewer's own
// writes (`Publish`). Any thread.
hudsection::Section Published ();
// MAIN THREAD: Archicad's selection's section read now, and published.
void Publish ();
// From Archicad's selection notification: published again from the message loop, once for a
// burst, while the viewer is open.
void SelectionChanged ();
// The project closed: nothing published is that project's.
void Forget ();

} // namespace sectionmodel
} // namespace archviz
} // namespace geomsrv

#endif
