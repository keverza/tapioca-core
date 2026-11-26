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
// ⚠️ A NO-GO ZONE KEEPS CROWNS OUT, IN PLAN. A path, an access route or the
// ground in front of a window, marked by the user: a plant taller than the
// zone's allowance may not stand where its crown disc (radius plus margin)
// would reach into the zone. It is the user's rule, never inferred.
//
// ⚠️ OVERLAPPING ZONES. A sample inside two zones counts in each zone's row;
// the "outside every zone" row counts it nowhere. Rows can therefore sum to more
// than the measured area, and the table says so when they do.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

enum class ZoneRole : uint8_t {
    Bed, // a planting zone: area rows, and the Scheme may be limited to it
    NoGo // crowns keep out (a path, access, a window's view)
};

struct PlantingZone {
    std::string guid;
    std::string name; // the element's ID, else "Fill" / "Polyline" and the GUID's tail
    bool fill = false;
    uint64_t stamp = 0;
    // Ring 0 is the outline, the rest are holes; x, y pairs, arcs tessellated,
    // not repeating the first point.
    std::vector<std::vector<double>> rings;
    ZoneRole role = ZoneRole::Bed;
    // NoGo: plants up to this mature height may stand inside (a low hedge under
    // a window); 0 keeps every plant out.
    float allowedHeight = 0.0f;

    double PlanArea () const; // outline less holes, m²
    bool Contains (double x, double y) const;
    // Plan distance from (x, y) to the zone: 0 inside, else to the nearest
    // outline or hole edge.
    double PlanDistance (double x, double y) const;
};

// For each sample and each no-go zone (in `zones` order, Bed zones skipped),
// the plan distance to it, sample-major; `allowance` gets each no-go zone's
// allowed height. Empty when there is no no-go zone.
void NoGoDistances (const std::vector<PlantingZone>& zones, const std::vector<double>& x, const std::vector<double>& y,
                    std::vector<float>& distance, std::vector<float>& allowance);

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
