#ifndef EVP_PALETTE_PROGRAMMEDIALOG_HPP
#define EVP_PALETTE_PROGRAMMEDIALOG_HPP

#include "ArchViz/HudFloorProgramme.hpp"

namespace evp::programmeui {
// MAIN THREAD, outside ImGui/render locks. No project writes; Apply returns a validated table.
bool Ask (const geomsrv::archviz::hudprogramme::TableEdit& edit, geomsrv::archviz::floorprogramme::Programme& answer);
} // namespace evp::programmeui
#endif
