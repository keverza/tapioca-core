#ifndef TAPIOCA_ARCHVIZ_PLANTINGZONES_HPP
#define TAPIOCA_ARCHVIZ_PLANTINGZONES_HPP

// ArchViz/PlantingZones -- planting zones drawn in plan (#278): closed
// Polylines and Fills read from the Archicad selection
// (PlantingZoneSource.hpp), each projected straight down onto the measured
// hosts. A zone limits the area table and the planting scheme to the samples
// inside it.
//
// Pure: plan rings only, no DevKit.
//
// ⚠️ A ZONE IS A PLAN OUTLINE, NOT A SURFACE. A sample belongs to a zone when
// its plan position is inside the outer ring and outside every hole; the host
// surface under it decides its height. A zone over no measured host has no
// samples, and says so, rather than an area of zero.
//
// ⚠️ OVERLAPPING ZONES. A sample inside two zones counts in each zone's row;
// the "outside every zone" row counts it nowhere. Rows can therefore sum to more
// than the measured area, and the table says so when they do.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

struct PlantingZone {
    std::string guid;
    std::string name; // the element's ID, else "Fill" / "Polyline" and the GUID's tail
    bool fill = false;
    uint64_t stamp = 0;
    // Ring 0 is the outline, the rest are holes; x, y pairs, arcs tessellated,
    // not repeating the first point.
    std::vector<std::vector<double>> rings;

    double PlanArea () const; // outline less holes, m²
    bool Contains (double x, double y) const;
};

// For each sample, the bit set of zones containing it (zone i is bit i; at most
// 32 zones). 0 = outside every zone.
std::vector<uint32_t> ZoneMasks (const std::vector<PlantingZone>& zones, const std::vector<double>& x,
                                 const std::vector<double>& y);
constexpr size_t kMaxPlantingZones = 32;

// Plan area per light class (raw hours, the exact comparator H >= edge) of
// the samples a zone contains, with the measured area and the count.
struct ZoneClassAreas {
    std::vector<double> classAreas; // edges.size () + 1 classes, lowest first
    double measured = 0.0;
    size_t samples = 0;
};
ZoneClassAreas ClassAreasInZone (const std::vector<uint32_t>& masks, uint32_t zoneBit, const std::vector<double>& hours,
                                 const std::vector<double>& areas, const std::vector<double>& edges);

} // namespace geomsrv::archviz::planting

#endif
