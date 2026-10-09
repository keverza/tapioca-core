#ifndef EVP_ARCHVIZ_FLOORPROGRAMME_HPP
#define EVP_ARCHVIZ_FLOORPROGRAMME_HPP

// The apartment programme: per flat type a room count, a NET area range and a share
// of the flat count. Shares are fractions that always add to one; the target area is
// the middle of the range, the minimum frontage 2.4 m + 1.2 m per room.
//
// The interchange is the brief text the private generator's fixtures carry
// ("30% 2 room 40-45m2", one type per line or ';'), so both read one contract.
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::floorprogramme {
constexpr size_t kMaxTypes = 12;
constexpr double kMinArea = 15, kMaxArea = 300, kMinRooms = 1, kMaxRooms = 8;
constexpr double kWall = 0.2;   // wall between flats, deducted once per flat across the band
constexpr double kFacade = 0.5; // external wall inside the counted outline
struct UnitType {
    double rooms = 2;
    double minM2 = 40, maxM2 = 45;
    double share = 0; // fraction of the flat count
    bool operator== (const UnitType&) const = default;
};
struct Programme {
    std::vector<UnitType> types;
    bool operator== (const Programme&) const = default;
};
// The user's brief (2026-10-08): 1.5R 30-36 5%, 2R 40-45 30%, 3R 55-65 20%,
// 3R 65-75 20%, 4R 75-80 20%, 5R 80-90 5%.
Programme Default ();
std::string Problem (const Programme& programme); // empty when valid
bool Valid (const Programme& programme);
double Target (const UnitType& type);
double Frontage (const UnitType& type);
std::string Name (const Programme& programme, size_t type); // "2R"; a repeated room count adds its range
std::string Key (const Programme& programme);               // cache identity
double MeanArea (const Programme& programme);               // share-weighted target
// Edits keep the shares adding to one: the other types scale in proportion.
bool SetShare (Programme& programme, size_t type, double share);
bool SetRooms (Programme& programme, size_t type, double rooms);
bool SetRange (Programme& programme, size_t type, double minM2, double maxM2);
size_t AddType (Programme& programme); // returns the new index, or kMaxTypes when full
bool RemoveType (Programme& programme, size_t type);
void Normalise (Programme& programme);
// Type whose target is closest to `area`.
size_t Nearest (const Programme& programme, double area);
// `current` while `area` is within its range, else the fitting type with the closest target.
size_t Retype (const Programme& programme, size_t current, double area);
// Largest-remainder integer counts of `flats` by share.
std::vector<int> Counts (const Programme& programme, int flats);
// Programme deviation: half weight within 3% per type, 4 per flat beyond it.
double MixCost (const Programme& programme, const std::vector<int>& counts);
uint32_t Colour (double rooms); // 0xRRGGBBAA
bool Parse (const std::string& text, Programme& programme, std::string& error);
// One brief line, its share kept as written (a fraction; not normalised).
bool ParseType (const std::string& text, UnitType& type, std::string& error);
// Types joined by `separator`: "; " for the one-line prompt, a newline for the generator's files.
std::string Brief (const Programme& programme, const char* separator = "; ");
// The stored form (the project's Add-On Object): exact, one type per line after a header.
std::string ToText (const Programme& programme);
bool FromText (const std::string& text, Programme& programme, std::string& error);
} // namespace geomsrv::archviz::floorprogramme
#endif
