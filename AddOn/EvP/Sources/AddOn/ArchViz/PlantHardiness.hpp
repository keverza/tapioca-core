#ifndef TAPIOCA_ARCHVIZ_PLANTHARDINESS_HPP
#define TAPIOCA_ARCHVIZ_PLANTHARDINESS_HPP

// ArchViz/PlantHardiness -- winter hardiness for the planting tab (#278,
// #155): USDA zones as half-zone indices, a plant's stated range
// (ecology.hardiness, "USDA 4a-8b"), and the zone a temperature falls in.
//
// Pure: no weather file, no ImGui.
//
// ⚠️ A WEATHER FILE DOES NOT GIVE THE ZONE. A USDA zone is the mean of many
// winters' lowest temperatures. A typical-year (TMY, IWEC) file is built from
// typical months and a normals file is averaged, so their coldest hour runs
// warmer -- often by one zone, sometimes more (the Long Island normals file
// reads 9b where the zone is about 7). The file's reading is offered as a
// suggestion; the design zone is the user's choice.
//
// ⚠️ HARDY IS ABOUT COLD ONLY. A site warmer than a plant's range is noted,
// not refused: the upper zone of a range is not a heat limit.

#include "SunStudy/SunStudyWeather.hpp"

#include <string>

namespace geomsrv::archviz::planting {

// Half-zone index: 0 = 1a, 1 = 1b, 2 = 2a, ... 25 = 13b. -1 = none.
constexpr int kUsdaHalfZones = 26;
std::string UsdaName (int halfZone); // "6a"; "" for -1

// The half zone a minimum temperature falls in: 5 °F bands from -60 °F
// (1a), with the exact comparator (-23.33 °C, -10 °F, is 6a). Clamped to 1a..13b.
int UsdaHalfZoneOf (double minimumC);

// "USDA 4a-8b", "USDA 4-8" (a whole zone is a to b), "4a–8b" (any dash);
// "USDA 5a", "USDA 4", "zone 6b": hardy down to that zone, no warm end (hi =
// 13b); "RHS H6": the cold end of the RHS band (-20 °C, 6b), no warm end.
// False, leaving lo/hi -1, for anything else.
bool ParseUsdaRange (const std::string& text, int& lo, int& hi);

enum class Hardiness : unsigned char {
    Unknown, // no range in the catalogue, or no design zone
    Hardy,   // the design zone is at or above the range's cold end
    TooCold, // the design zone is colder than the range's cold end
    Warmer   // hardy, and the site is warmer than the range's warm end
};
Hardiness HardinessAt (const std::string& range, int designHalfZone);

// The weather file's coldest valid dry-bulb hour, °C. False when it has none.
bool ColdestHourC (const evp::sunstudy::WeatherData& weather, double& coldestC);

} // namespace geomsrv::archviz::planting

#endif
