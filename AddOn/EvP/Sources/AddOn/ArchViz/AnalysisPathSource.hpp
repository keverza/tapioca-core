#ifndef EVP_ARCHVIZ_ANALYSISPATHSOURCE_HPP
#define EVP_ARCHVIZ_ANALYSISPATHSOURCE_HPP

// ArchViz/AnalysisPathSource -- reads the path for the shared analysis point
// (AnalysisPath.hpp) from the Polyline selected in Archicad. The HUD asks from the
// render thread; the read runs later on the main thread (SelectionMetadata's
// `Later`), and the HUD picks the result up by its serial.
//
// ANY THREAD: RequestImport, Latest.

#include "ArchViz/AnalysisPath.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace geomsrv::archviz::analysispathsource {

// Read the first Polyline in the Archicad selection, at its storey's level.
void RequestImport ();

struct Imported {
    uint64_t serial = 0;                            // grows with each finished import, read or not
    std::shared_ptr<const analysispath::Path> path; // null when it failed
    std::string error;
};

Imported Latest ();

} // namespace geomsrv::archviz::analysispathsource

#endif
