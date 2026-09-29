#ifndef EVP_ARCHVIZ_STORYSLICEOVERLAY_HPP
#define EVP_ARCHVIZ_STORYSLICEOVERLAY_HPP

// ArchViz/StorySliceOverlay -- the slices on the overlays, switched by
// Tapioca.OverlayStorySlices, as the add-on's own layer (`tapioca.storeySlices`,
// StorySliceOverlayContent.hpp). Two sources:
//
//   selection / elements  the massing slabs: each slab's floors under a rule
//                         (SlabSlices.hpp), read from the database (SlabSliceSource.hpp).
//                         `on` takes the slabs selected at that moment, or the ones named;
//                         while on, an edit to one of them, or to the storeys, re-cuts
//                         within a tick, and `refresh` re-reads them all.
//   model                 the whole model cut at each storey: the extraction's union
//                         outlines, from a full pass (StorySliceAccumulator). An element
//                         edit re-extracts incrementally and leaves them as they were;
//                         `refresh` asks for a full pass.
//
// MAIN THREAD. A 500 ms WM_TIMER while enabled: low priority, so it does not fire
// during a drag or a heavy edit, and ACAPI is legal in it (ModelWatch.hpp). Its ACAPI is
// one header read per slab and one storey read. `Shutdown` kills it without ACAPI.

#include "ArchViz/SlabSliceSource.hpp"
#include "ArchViz/StorySliceOverlayContent.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace storysliceoverlay {

enum class Source { Selection, Elements, Model };

// "selection", "elements", "model".
const char* SourceName (Source source);

struct Request {
    Source source = Source::Selection;
    std::vector<std::string> elements; // Source::Elements: element GUIDs
    slabslices::Rule rule;
};

struct State {
    bool enabled = false;
    Source source = Source::Selection;
    slabslices::Cut cut = slabslices::Cut::Storeys;
    bool waiting = false;  // model: enabled and no cut yet, a full pass is asked for or running
    uint32_t slices = 0;   // drawn under the filter
    double areaM2 = 0.0;   // their areas summed
    uint32_t storeys = 0;  // model: storeys in the latest cut
    uint64_t snapshot = 0; // model: the cut drawn
    uint32_t cuts = 0;     // times the slices were rebuilt; an edit to a slab moves it
    std::vector<slabslices::Summary> slabs;
    std::vector<slabsource::Skip> skipped;
    std::string message;
};

// On with this request and these controls, or off. `refresh` re-reads the slabs taken
// (selection, elements) or asks for a new full pass (model); `on` takes the selection
// anew.
State Apply (bool enabled, const Request& request, const Controls& controls, bool refresh);
State Describe ();

// The project closed: off, its slabs and cuts forgotten (§8).
void OnProjectClosed ();
// The add-on unloads: the timer goes, nothing else is called.
void Shutdown ();

} // namespace storysliceoverlay
} // namespace archviz
} // namespace geomsrv

#endif
