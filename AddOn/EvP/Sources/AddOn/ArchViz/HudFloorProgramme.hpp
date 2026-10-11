#ifndef EVP_ARCHVIZ_HUDFLOORPROGRAMME_HPP
#define EVP_ARCHVIZ_HUDFLOORPROGRAMME_HPP

// The Massing tab's Define Programme section: flat types with room count, net area range
// and share, the shares kept at 100%. Drawing never calls Archicad; typed values go to the
// owner's native Programme table (the overlay cannot take the keyboard) and come back by Answer. A
// finished edit asks the owner to store the programme in the project.
#include "ArchViz/FloorProgramme.hpp"
#include <string>
#include <vector>

namespace geomsrv::archviz::hudprogramme {
struct TextEdit {
    floorprogramme::Programme before;
    int type = -1; // -1: the whole brief; else one row
    std::string text;
};
enum class Field { Rooms, Minimum, Maximum, Share };
struct TableEdit {
    floorprogramme::Programme before;
    size_t type = 0;
    Field field = Field::Rooms;
};
struct ColourEdit {
    floorprogramme::Programme before;
    size_t type = 0;
};
struct Outcome {
    bool changed = false; // `programme` changed this frame (previews follow it live)
    bool save = false;    // an edit finished: store `programme` in the project
};
// `saved` is the project's stored programme, for the section's saved / not saved line.
Outcome Draw (floorprogramme::Programme& programme, const floorprogramme::Programme& saved,
              std::vector<TableEdit>& tables, std::vector<ColourEdit>& colours);
// Native table readback: validate each value before normalising the share percentages.
bool PrepareTable (floorprogramme::Programme values, floorprogramme::Programme& answer, std::string& error);
bool Answer (floorprogramme::Programme& programme, const TableEdit& edit, const floorprogramme::Programme& answer,
             std::string& error);
bool Answer (floorprogramme::Programme& programme, const ColourEdit& edit, uint32_t rgba, std::string& error);
// A prompt's answer: false with `error` when the programme moved on or the text is wrong.
bool Answer (floorprogramme::Programme& programme, const TextEdit& edit, const std::string& answer, std::string& error);
} // namespace geomsrv::archviz::hudprogramme
#endif
