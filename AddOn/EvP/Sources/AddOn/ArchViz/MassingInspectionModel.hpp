#ifndef EVP_ARCHVIZ_MASSINGINSPECTIONMODEL_HPP
#define EVP_ARCHVIZ_MASSINGINSPECTIONMODEL_HPP
#include "ArchViz/HudSection.hpp"
namespace geomsrv::archviz::massinginspectionmodel {
// Main-thread only. Reuses immutable massing snapshots; no model writes or extraction.
void Follow (bool unique, const std::string& building, const std::string& floorBuilding, const hudsection::Run& floors);
void Refresh ();
std::string Note ();
void Forget ();
} // namespace geomsrv::archviz::massinginspectionmodel
#endif
