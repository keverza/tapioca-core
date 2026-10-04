#ifndef EVP_ARCHVIZ_HUDSECTION_HPP
#define EVP_ARCHVIZ_HUDSECTION_HPP

// ArchViz/HudSection -- the building section on the HUD's Selection page (the user,
// 2026-10-03): the selected massing slabs' floors stacked as a schematic section, to pick
// floors and give them their part of the element's information scheme.
//
//   Archicad's slabs -> the floors they are cut into (SlabSlices.hpp) -> a floor's metadata
//   (a range over the "floor" domain, TapiocaMetadata.hpp) -> this diagram
//
// ⚠️ A FLOOR IS A STOREY: its position in the floor domain is the Archicad storey number it lies
// in (slabslices::StoreysAt), so "floor 0 commerce, floors 1-7 residential" means the ground
// storey and the seven above, whichever slab holds them. Every floor is a row of one height;
// its bar is as wide as its area beside the widest floor's -- a podium reads wider than its
// tower -- and the slabs at one storey stand side by side in it, each in the colour its value
// has (the schema's first enumeration that may be assigned over floors: program.usage's
// zoning colours by default).
//
// ⚠️ PICKING: a press picks a floor, a shift-press or a drag a run of them; a right click opens
// the values to assign to the run -- written to each slab over its own floors of it, one undo
// step (SelectionMetadata.hpp). The run is the caller's to keep; the owner draws its floors'
// slices on the 3D overlay.
//
// Pure: ImGui and the models. tests/cpp builds it.

#include "ArchViz/ExtractionStorySlices.hpp" // ProjectStoreys
#include "ArchViz/HudMetadata.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/SlabSlices.hpp"
#include "Metadata/TapiocaMetadata.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace hudsection {

// One slab at one storey.
struct Part {
    std::string guid;
    double areaM2 = 0.0;
    std::string value; // its value of the section's key there; empty: none
    uint32_t rgba = 0; // that value's colour; alpha 0: none
};

struct Floor {
    int storey = 0;    // its position in the floor domain
    std::string label; // the storey's name, or its number
    double base = 0.0; // world metres: the lowest of its slabs'
    double areaM2 = 0.0;
    std::vector<Part> parts;
};

// Each slab's own floors: what a run is clipped to for it.
struct Span {
    std::string guid;
    int low = 0;
    int high = -1;
};

struct Section {
    bool known = false;
    std::string key; // the definition it colours by and assigns: "program.usage"
    std::string keyLabel;
    metadata::ValueType valueType = metadata::ValueType::Enum;
    std::vector<hudmeta::Option> options;
    std::vector<Floor> floors; // the lowest first
    std::vector<Span> spans;
    double widestM2 = 0.0;
    std::string note; // what could not be read
};

// A run of floors, by storey; `first` > `last` is none.
struct Run {
    int first = 0;
    int last = -1;
    bool Empty () const
    {
        return last < first;
    }
    bool Has (int storey) const
    {
        return storey >= first && storey <= last;
    }
};
bool operator== (const Run& a, const Run& b);
inline bool operator!= (const Run& a, const Run& b)
{
    return !(a == b);
}

// One slab as the section reads it: its floors (bottom first), each one's storey, its metadata.
struct Slab {
    std::string guid;
    std::vector<slabslices::Floor> floors;
    std::vector<int> storeys;
    metadata::EntityMetadata meta;
};

// The section of `slabs` under `schema`, its rows named from `storeys`.
Section Build (const std::vector<Slab>& slabs, const ProjectStoreys& storeys, const metadata::ProjectSchema& schema,
               const std::string& property = {});

// The edits that assign `value` (or clear the key, `clear`) over `run`, one per slab that has a
// floor in it, clipped to that slab's own floors.
std::vector<hudmeta::Edit> RunEdits (const Section& section, const Run& run, const std::string& value, bool clear);

// The section drawn in `look`; `run` the floors picked, kept by the caller and changed here.
// What the user assigned in this frame.
std::vector<hudmeta::Edit> Diagram (const Section& section, Run& run, const overlaylayers::Panel& look, float scale);

} // namespace hudsection
} // namespace archviz
} // namespace geomsrv

#endif
