#ifndef EVP_ARCHVIZ_MASSINGRULESMODEL_HPP
#define EVP_ARCHVIZ_MASSINGRULESMODEL_HPP

#include "ArchViz/MassingRules.hpp"

namespace geomsrv::archviz::massingrulesmodel {
// MAIN THREAD. Borrow the model database and restore it on every exit. Apply is
// called only from the posted HUD request, never from layout or a native command.
massingrules::Page Read (const std::vector<std::string>& guids);
std::vector<massingrules::Page> ReadParcels (const std::vector<std::string>& guids);
bool Apply (const massingrules::Edit& edit, std::string& error);
} // namespace geomsrv::archviz::massingrulesmodel
#endif
