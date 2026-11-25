#ifndef EVP_SUNSTUDY_SUNSTUDYPLANTINGAREAS_HPP
#define EVP_SUNSTUDY_SUNSTUDYPLANTINGAREAS_HPP

// SunStudy/SunStudyPlantingAreas -- the Planting tab's area table (#278): plan
// area per sunlight class for each host and in total, with the unavailable area
// beside it, split by why.
//
// ⚠️ EXACT CLASSES FROM THE RAW HOURS (SunStudyLightClass), per host: a value at
// an edge belongs to the upper class, and unavailable area is never shade. A
// host is a grid group -- the snapshot mesh, one Archicad element.

#include "SunStudy/SunStudyReceptor.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace evp::sunstudy {

struct PlantingAreaRow {
    uint32_t group = 0;             // the host mesh; unused in the total
    std::vector<double> classAreas; // m2 per class, lowest first
    double evaluatedArea = 0.0;     // sum of classAreas
    double steepArea = 0.0;         // unavailable: host too steep
    double blockedArea = 0.0;       // unavailable: ceiling too close overhead
    double invalidArea = 0.0;       // unavailable: no usable host
    size_t samples = 0, unclassified = 0;
    double UnavailableArea () const
    {
        return steepArea + blockedArea + invalidArea;
    }
};

struct PlantingAreaTable {
    std::vector<PlantingAreaRow> hosts; // ascending group
    PlantingAreaRow total;
};

// `hours`, `areas` and `groups` are parallel per sample; `losses` comes from the
// study's receptor layer (empty for none). A host with only unavailable area
// still gets a row. False, with `error`, for mismatched arrays or refused edges.
bool BuildPlantingAreaTable (const std::vector<double>& hours, const std::vector<double>& areas,
                             const std::vector<uint32_t>& groups, const std::vector<ReceptorHostLoss>& losses,
                             const std::vector<double>& edges, PlantingAreaTable& out, std::string& error);

} // namespace evp::sunstudy

#endif
