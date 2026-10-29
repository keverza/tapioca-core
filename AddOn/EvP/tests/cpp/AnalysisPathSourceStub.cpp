// Link stub for the Archicad half of the analysis path.
//
// ArchViz/AnalysisPathSource.cpp reads a Polyline through ACAPI and can never
// join the offline build, while DiligentHudViewpoint.cpp -- which the offline
// gate DOES compile -- asks it for imports. This answers as production does
// before any import: nothing taken yet. Paths are tested through the pure
// ArchViz/AnalysisPath.cpp instead.
//
// ⚠️ ADD A FUNCTION HERE ONLY WHEN A LINKED SOURCE CALLS IT.

#include "ArchViz/AnalysisPathSource.hpp"

namespace geomsrv::archviz::analysispathsource {

void RequestImport ()
{
}

Imported Latest ()
{
    return {};
}

} // namespace geomsrv::archviz::analysispathsource
