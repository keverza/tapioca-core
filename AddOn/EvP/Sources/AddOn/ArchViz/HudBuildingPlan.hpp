#ifndef EVP_ARCHVIZ_HUDBUILDINGPLAN_HPP
#define EVP_ARCHVIZ_HUDBUILDINGPLAN_HPP

#include "ArchViz/FloorProgramme.hpp"
#include "ArchViz/FloorSchemeDesigns.hpp"
#include "ArchViz/HudSection.hpp"
#include <limits>
#include <map>
#include <set>

namespace geomsrv::archviz::massingslices {
struct Result;
}
namespace geomsrv::archviz::massingbuildings {
struct Preview;
}
namespace geomsrv::archviz::buildingplan {
constexpr char kLocations[] = "massing.stairwellLocations";
constexpr char kShapes[] = "massing.stairwellShapes";
constexpr const char* kDesigns = floorscheme::edit::kDesignsKey; // the floor designs, beside the stairs
constexpr size_t kMaxStairs = 32;
// Stair assembly: 4.5 m along the corridor, 4.2 m from its facade (three flights around the lift).
constexpr double kStairWidth = 4.5, kStairDepth = 4.2;
constexpr double kMinCore = 2.0, kMaxCore = 12.0;
constexpr char kStairsLayer[] = "tapioca.massing.proposedStairs";
constexpr char kUnitsLayer[] = "tapioca.massing.proposedUnits";
struct Point {
    double x = 0, y = 0;
    bool operator== (const Point&) const = default;
};
// A proposed stair core: centre in project XY, width along and depth across the building
// frame. The corridor reaches it at the middle of the side that faces the corridor.
struct Core {
    Point center;
    double width = kStairWidth, depth = kStairDepth;
    bool operator== (const Core&) const = default;
};
struct Stored {
    std::vector<Core> cores;
    std::string designs; // floorscheme::edit::Designs as JSON; empty when none were saved
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
    double height = 0;
    double areaM2 = 0;
    // Keep each source's holes paired with its outer rings: overlap between
    // different slabs is a union, not an even/odd cancellation.
    std::vector<std::vector<SliceChain>> contours, physical;
    std::vector<SliceChain> outline; // Unioned counted boundary, including holes; no internal source seams.
    bool outlineKnown = false;
    std::string outlineKey; // Canonical world boundary identity, assembled once with the immutable snapshot.
};
struct Plan {
    std::string key;
    std::vector<std::string> guids;
    std::vector<Source> sources;
    std::vector<Floor> floors;
    std::vector<Core> saved;
    std::string savedDesigns; // every member's, when they agree
    double angle = 0;         // building frame: the longest counted edge of the largest floor
    bool mixed = false;
    std::string note;
};
struct Draft {
    bool known = false;
    int story = (std::numeric_limits<int>::min) (); // the floor shown (picked in the story section)
    std::vector<Core> original, cores;
    bool changed = false;
    bool originalMixed = false;
    std::vector<std::string> guids;
    std::vector<std::string> fingerprints;
    floorprogramme::Programme programme = floorprogramme::Default (); // the Massing programme, HUD-session
    // The floor designs (stairs aside: `cores`), and as saved; Save writes them with the stairs.
    floorscheme::edit::Designs designs, originalDesigns;
    std::string previewFingerprint;
};
Stored Read (const metadata::EntityMetadata& entity);
std::string Fingerprint (const metadata::EntityMetadata& entity);
bool Matches (const metadata::EntityMetadata& entity, const hudmeta::Edit& edit);
Plan Build (const massingslices::Result& slices, const massingbuildings::Preview& preview);
const Floor* Displayed (const Plan& plan, const Draft& draft);
bool Contains (const Floor& floor, Point point);
// World corners of a core turned to the building frame.
std::vector<Point> Corners (const Core& core, double angle = 0);
// A floor's outline identity in world XY: equal for identical outlines (whatever their start
// vertex, winding or order), never for translated ones; height and index are not part of it.
std::string OutlineKey (const Floor& floor);
void UseProgramme (Draft& draft, const floorprogramme::Programme& programme);
bool Dirty (const Draft& draft);
bool Conflict (const Plan& plan, const Draft& draft);
void Reset (const Plan& plan, Draft& draft);
void Sync (const Plan& plan, Draft& draft);
// A plan example for offline reference: the building's counted floors as a story-slices file
// (tapioca.story-slices.2d, which the private generator loads as a fixture) with the programme
// as its brief and a "plan" section holding the cores and every floor design's flats.
struct PlanFile {
    std::string name, text;
};
// `schemes`: each floor's scheme as last planned, by story (a floor not yet planned is left out).
PlanFile ExportPlan (const Plan& plan, const Draft& draft, const Floor& shown, const std::string& stamp,
                     const std::map<int, const floorscheme::Scheme*>& schemes);
// Pure targeted edits; the owner writes outside ImGui and checks building identity.
std::vector<hudmeta::Edit> Edits (const Plan& plan, const Draft& draft);
} // namespace geomsrv::archviz::buildingplan
#endif
