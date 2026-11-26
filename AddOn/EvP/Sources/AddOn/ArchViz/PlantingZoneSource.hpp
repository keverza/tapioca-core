#ifndef TAPIOCA_ARCHVIZ_PLANTINGZONESOURCE_HPP
#define TAPIOCA_ARCHVIZ_PLANTINGZONESOURCE_HPP

// ArchViz/PlantingZoneSource -- reads planting zones (PlantingZones.hpp) from
// the closed Polylines and Fills selected in Archicad (#278). The HUD asks from
// the render thread; the read runs later on the main thread (SelectionMetadata's
// `Later`, as AnalysisPathSource does), and the HUD picks the result up by its
// serial.
//
// An open Polyline is skipped and named in `skipped`: a zone needs an inside.
//
// ANY THREAD: RequestImport, Latest.

#include "ArchViz/PlantingZones.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::plantingzonesource {

// Read every closed Polyline and Fill in the Archicad selection.
void RequestImport ();

struct Imported {
    uint64_t serial = 0; // grows with each finished import, read or not
    std::vector<planting::PlantingZone> zones;
    std::vector<std::string> skipped; // "Polyline ..1A2B: not closed"
    std::string error;                // empty when the selection was read
};
Imported Latest ();

} // namespace geomsrv::archviz::plantingzonesource

#endif
