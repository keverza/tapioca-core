#ifndef EVP_ARCHVIZ_STORYSLICESNAPSHOT_HPP
#define EVP_ARCHVIZ_STORYSLICESNAPSHOT_HPP

// ArchViz/StorySliceSnapshot -- the storey cuts of the last full extraction pass, PER
// STOREY: its level, its name, the union outline's contours and the area they
// enclose. The viewer's StorySliceLayer receives one merged ribbon for the whole
// building; the overlays need each storey apart -- a label per slice, a storey filter
// -- so the pass publishes this beside it (ExtractionStorySlices.cpp).
//
// ⚠️ PUBLISHED FROM THE EXTRACTION WORKER, READ ON THE MAIN THREAD. One mutex around
// one shared pointer, off every hot path: the worker publishes once per full pass, the
// main thread copies the pointer on its tick. Snapshots never change once published.
//
// ⚠️ A FULL PASS ONLY, AND ONLY ONE THAT FINISHED (StorySliceAccumulator's rule): a
// union over part of a storey is a confident, clean, wrong outline.

#include "ArchViz/StorySliceGeometry.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace storeyslices {

struct Storey {
    int index = 0; // Archicad's storey number
    double level = 0.0;
    std::string name;
    std::vector<SliceChain> chains; // the union outline, x y metres
    double areaM2 = 0.0;
};

struct Snapshot {
    std::vector<Storey> storeys; // the storeys the model reaches, bottom to top
    uint64_t generation = 0;
};

// Any thread (the extraction worker): a finished full pass's cuts replace the last.
void Publish (std::vector<Storey> storeys);

// Any thread: the latest, or null before any pass cut a storey.
std::shared_ptr<const Snapshot> Latest ();

// A project close: its storeys are not the next project's (§8).
void Clear ();

} // namespace storeyslices
} // namespace archviz
} // namespace geomsrv

#endif
