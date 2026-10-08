#ifndef EVP_ARCHVIZ_HUDBUILDINGPLAN_HPP
#define EVP_ARCHVIZ_HUDBUILDINGPLAN_HPP

#include "ArchViz/HudSection.hpp"

namespace geomsrv::archviz::massingslices {
struct Result;
}
namespace geomsrv::archviz::massingbuildings {
struct Preview;
}
namespace geomsrv::archviz::buildingplan {
constexpr char kLocations[] = "massing.stairwellLocations";
constexpr size_t kMaxStairs = 32;
struct Point {
    double x = 0, y = 0;
    bool operator== (const Point&) const = default;
};
struct Stored {
    std::vector<Point> points;
    bool invalid = false;
};
struct Source {
    std::string guid;
    Stored stored;
    std::string fingerprint;
};
struct Floor {
    int story = 0;
    double z = 0;
    double areaM2 = 0;
    // Keep each source's holes paired with its outer rings: overlap between
    // different slabs is a union, not an even/odd cancellation.
    std::vector<std::vector<SliceChain>> contours, physical;
};
struct Plan {
    std::string key;
    std::vector<std::string> guids;
    std::vector<Source> sources;
    std::vector<Floor> floors;
    std::vector<Point> saved;
    bool mixed = false;
    std::string note;
};
struct Draft {
    bool known = false, placing = false;
    int selected = -1; // -1: append a new stair; otherwise move the selected marker.
    int story = (std::numeric_limits<int>::min) ();
    std::vector<Point> original, points;
    bool changed = false;
    bool originalMixed = false;
    std::vector<std::string> guids;
    std::vector<std::string> fingerprints;
};
Stored Read (const metadata::EntityMetadata& entity);
std::string Fingerprint (const metadata::EntityMetadata& entity);
bool Matches (const metadata::EntityMetadata& entity, const hudmeta::Edit& edit);
Plan Build (const massingslices::Result& slices, const massingbuildings::Preview& preview);
const Floor* Displayed (const Plan& plan, const Draft& draft);
bool Contains (const Floor& floor, Point point);
bool Place (const Floor& floor, Draft& draft, Point point);
bool NeedsTwoStairs (double areaM2, const massingareas::Coefficients& coefficients);
bool Dirty (const Draft& draft);
bool Conflict (const Plan& plan, const Draft& draft);
void Reset (const Plan& plan, Draft& draft);
// Pure targeted edits; the owner writes outside ImGui and checks building identity.
std::vector<hudmeta::Edit> Edits (const Plan& plan, const Draft& draft);
std::vector<hudmeta::Edit> Draw (const Plan& plan, Draft& draft, float scale,
                                 const massingareas::Coefficients& coefficients);
} // namespace geomsrv::archviz::buildingplan
#endif
