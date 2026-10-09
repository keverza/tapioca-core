#ifndef EVP_ARCHVIZ_HUDFLOORPLANFILES_HPP
#define EVP_ARCHVIZ_HUDFLOORPLANFILES_HPP

// The owner's half of Plan view's Export plan: the HUD builds the document
// (buildingplan::ExportPlan), this writes it under %LOCALAPPDATA%\Tapioca\plans and says where
// in the console. MAIN THREAD, from the message loop; never inside a HUD frame.
#include "ArchViz/HudBuildingPlan.hpp"

namespace geomsrv::archviz::planfiles {
void Write (const buildingplan::PlanFile& file);
} // namespace geomsrv::archviz::planfiles
#endif
