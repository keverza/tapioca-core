#ifndef EVP_ARCHVIZ_MASSINGMODEL_HPP
#define EVP_ARCHVIZ_MASSINGMODEL_HPP

#include "ArchViz/HudMassing.hpp"

namespace geomsrv::archviz::massingmodel {
// MAIN THREAD. Reads cache on demand; scans the model database, not the active
// worksheet. User edits are posted to the message loop, outside ImGui's lock.
hudmassing::Page Read ();
void Request (hudmassing::Request request);
void RequestRules (massingrules::Edit edit);
// Store the flat programme in the project, one undo step, on the message loop.
void RequestProgramme (floorprogramme::Programme programme);
void Changed ();
void Forget ();
} // namespace geomsrv::archviz::massingmodel
#endif
