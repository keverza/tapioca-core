#ifndef EVP_ARCHVIZ_HUDFLOORPROGRAMME_HPP
#define EVP_ARCHVIZ_HUDFLOORPROGRAMME_HPP

// The Massing tab's Define Programme section: flat types with room count, net area range
// and share, the shares kept at 100%. Drawing never calls Archicad; typed values go to the
// owner's native prompt (the overlay cannot take the keyboard) and come back by Answer. A
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
struct Outcome {
    bool changed = false; // `programme` changed this frame (previews follow it live)
    bool save = false;    // an edit finished: store `programme` in the project
};
// `saved` is the project's stored programme, for the section's saved / not saved line.
Outcome Draw (floorprogramme::Programme& programme, const floorprogramme::Programme& saved,
              std::vector<TextEdit>& prompts);
// A prompt's answer: false with `error` when the programme moved on or the text is wrong.
bool Answer (floorprogramme::Programme& programme, const TextEdit& edit, const std::string& answer, std::string& error);
} // namespace geomsrv::archviz::hudprogramme
#endif
