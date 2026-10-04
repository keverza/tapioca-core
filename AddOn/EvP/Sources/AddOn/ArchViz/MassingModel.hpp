#ifndef EVP_ARCHVIZ_MASSINGMODEL_HPP
#define EVP_ARCHVIZ_MASSINGMODEL_HPP

#include "ArchViz/HudMassing.hpp"

namespace geomsrv::archviz::massingmodel {
// MAIN THREAD. Reads cache on demand; scans the model database, not the active
// worksheet. User edits are posted to the message loop, outside ImGui's lock.
hudmassing::Page Read ();
void Request (hudmassing::Request request);
void RequestRules (massingrules::Edit edit);
void Changed ();
void Forget ();
} // namespace geomsrv::archviz::massingmodel
#endif
