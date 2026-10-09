#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/HudFloorPlanFiles.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp"
#include "Python/PathUtils.hpp"
#include <fstream>
#include <string>

namespace geomsrv::archviz::planfiles {
void Write (const buildingplan::PlanFile& file)
{
    const auto root = evp::EvpDataDir ();
    if (root.IsEmpty () || file.name.empty () || file.name.find_first_of ("\\/:") != std::string::npos) {
        hudconsole::Warning ("Plan view", "The plan was not exported: the local Tapioca data folder is unavailable.");
        return;
    }
    const GS::UniString directory (root + GS::UniString ("\\plans"));
    evp::CreateDirectoryChain (directory);
    const GS::UniString path (directory + GS::UniString ("\\") + GS::UniString (file.name.c_str (), CC_UTF8));
    const std::string shown (path.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    std::ofstream out (std::wstring (reinterpret_cast<const wchar_t*> (path.ToUStr ().Get ())),
                       std::ios::binary | std::ios::trunc);
    out << file.text;
    out.close ();
    if (!out) {
        hudconsole::Warning ("Plan view", "The plan could not be written to " + shown + ".");
        return;
    }
    ArchVizLog ("PLAN EXPORT  " + shown + " (" + std::to_string (file.text.size ()) + " bytes)");
    hudconsole::Say (hudconsole::Level::Note, "Plan view", "Plan exported to " + shown);
}
} // namespace geomsrv::archviz::planfiles
