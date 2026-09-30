// ArchViz/ArchVizLog for the offline suite. The add-on writes its log through Archicad's
// own file API into its data folder; here a line goes nowhere, so a source that narrates
// what it did (OverlayInput.cpp) can be tested for what it does.

#include "ArchViz/ArchVizLog.hpp"

namespace geomsrv {
namespace archviz {

void ArchVizLog (const std::string&)
{
}

void ArchVizLogClose ()
{
}

} // namespace archviz
} // namespace geomsrv
