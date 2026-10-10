#ifndef EVP_ARCHVIZ_FLOORSCHEMEDESIGNS_HPP
#define EVP_ARCHVIZ_FLOORSCHEMEDESIGNS_HPP

// A building's floor designs as the project keeps them (user, 2026-10-10: "there was no way to
// save the plan"): one Design per floor that leads a group of identical outlines, the floors made
// unique, as JSON text in the building's `massing.floorDesigns` metadata beside its stairwells.
// Pure: no ImGui, no Archicad.
#include "ArchViz/FloorSchemeEdit.hpp"
#include <map>
#include <set>
#include <string>

namespace geomsrv::archviz::floorscheme::edit {
constexpr char kDesignsKey[] = "massing.floorDesigns";
constexpr size_t kMaxDesignsText = 256 * 1024;

struct Designs {
    std::map<int, Design> floors; // by the story whose design a floor uses (shared or unique)
    std::set<int> unique;         // floors with a design of their own though their outline repeats
    bool operator== (const Designs&) const = default;
    bool Empty () const
    {
        return floors.empty () && unique.empty ();
    }
};

// Compact JSON; empty designs write as an empty string.
std::string ToJson (const Designs& designs);
// False with `error` (and `designs` untouched) for text that is not a designs document.
bool FromJson (const std::string& text, Designs& designs, std::string& error);
} // namespace geomsrv::archviz::floorscheme::edit
#endif
