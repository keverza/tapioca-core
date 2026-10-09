#ifndef EVP_ARCHVIZ_HUDBUILDINGPLAN_HPP
#define EVP_ARCHVIZ_HUDBUILDINGPLAN_HPP

#include "ArchViz/HudSection.hpp"
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
constexpr size_t kMaxStairs = 32;
constexpr double kStairWidth = 4.5, kStairDepth = 4.1, kSnapDistance = 0.5;
constexpr char kStairsLayer[] = "tapioca.massing.proposedStairs";
constexpr char kUnitsLayer[] = "tapioca.massing.proposedUnits";
struct Point {
    double x = 0, y = 0;
    bool operator== (const Point&) const = default;
};
// Native S0-S5 quick scheme, not the Python optimiser or a compliance result.
struct PlanRegion {
    std::vector<SliceChain> rings;
    Point center;
    bool alongX = true;
    double lo = 0, hi = 0, across = 0;
    int endAccess = 0; // -1/+1 when corridor access is only at the low/high axis end.
    struct AreaSpan {
        double lo, hi, width, slope, before;
    };
    std::vector<AreaSpan> areaProfile;
    std::vector<Point> triangles; // Cached hole-aware fill triangles, in world XY.
};
struct UnitSeed {
    uint32_t id = 0;
    size_t segment = 0;
    double along = 0;
    double rooms = 2;
    bool locked = false;
    double lo = 0, hi = 0; // Solved shared cuts in the segment's local axis.
};
struct QuickPlan {
    std::string signature, note;
    std::string outlineSignature;
    std::string solveNote;
    Point origin;
    double angle = 0;
    std::vector<PlanRegion> bars, corridors, bands, segments, units;
    std::vector<UnitSeed> seeds;
    uint32_t nextId = 1;
    int stage = 6, selected = -1;
    bool adding = false, dragging = false;
    uintptr_t owner = 0;
    double dragOriginal = 0;
    Point dragOffset;
    std::vector<UnitSeed> dragSeeds;
    std::vector<PlanRegion> dragUnits;
    double newRooms = 2;
    bool ready = false;
    uint64_t revision = 0;
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
    std::vector<Point> saved;
    bool mixed = false;
    std::string note;
};
struct Draft {
    bool known = false, placing = false;
    bool moving = false, dragging = false;
    uintptr_t dragOwner = 0; // Only the initiating ImGui context consumes the held-pointer gesture.
    Point dragOffset, dragOriginal;
    int selected = -1; // -1: append a new stair; otherwise move the selected marker.
    int story = (std::numeric_limits<int>::min) ();
    std::vector<Point> original, points;
    bool changed = false;
    bool originalMixed = false;
    std::vector<std::string> guids;
    std::vector<std::string> fingerprints;
    std::map<int, QuickPlan> quickPlans; // Local per-floor design; never written by Save stairwells.
    std::set<int> uniqueFloors;
    std::string previewFingerprint;
};
Stored Read (const metadata::EntityMetadata& entity);
std::string Fingerprint (const metadata::EntityMetadata& entity);
bool Matches (const metadata::EntityMetadata& entity, const hudmeta::Edit& edit);
Plan Build (const massingslices::Result& slices, const massingbuildings::Preview& preview);
const Floor* Displayed (const Plan& plan, const Draft& draft);
bool Contains (const Floor& floor, Point point);
bool Fits (const Floor& floor, Point center);
Point Snap (const Floor& floor, Point center);
std::string QuickSignature (const Floor& floor, const std::vector<Point>& stairs);
QuickPlan GenerateQuick (const Floor& floor, const std::vector<Point>& stairs);
Point UnitCenter (const QuickPlan& plan, const UnitSeed& seed);
bool AddUnit (QuickPlan& plan, Point point);
bool MoveUnit (QuickPlan& plan, size_t seed, Point point);
bool RemoveUnit (QuickPlan& plan, size_t seed);
double UnitTargetArea (double rooms);
uint32_t UnitColour (double rooms);
double UnitArea (const PlanRegion& unit);
bool SetUnitRooms (QuickPlan& plan, size_t seed, double rooms);
bool SetUnitLocked (QuickPlan& plan, size_t seed, bool locked);
void BuildAreaProfile (const QuickPlan& plan, PlanRegion& segment);
double AreaBefore (const PlanRegion& segment, double along);
double AlongAtArea (const PlanRegion& segment, double area);
bool RelaxUnits (QuickPlan& plan, int anchor = -1, bool exactTarget = false, bool dragging = false);
bool ChangeUnitTarget (const Plan& plan, Draft& draft, const Floor& floor, size_t seed, double rooms, bool locked);
bool TransferUnits (const QuickPlan& old, QuickPlan& next, int anchor = -1);
void PartitionUnits (QuickPlan& plan);
void RebuildUnits (QuickPlan& plan);
void CancelUnits (QuickPlan& plan);
QuickPlan& QuickFor (const Plan& plan, Draft& draft, const Floor& floor);
void MakeUnique (const Plan& plan, Draft& draft, const Floor& floor);
void ResetQuick (const Plan& plan, Draft& draft, const Floor& floor);
void PreviewLayers (const Plan& plan, Draft& draft, overlaylayers::Layer& stairs, overlaylayers::Layer& units,
                    bool includeUnits = true);
bool Place (const Floor& floor, Draft& draft, Point point);
bool BeginDrag (Draft& draft, Point mouse, uintptr_t owner = 0);
bool Drag (const Floor& floor, Draft& draft, Point mouse);
void EndDrag (Draft& draft);
void Cancel (Draft& draft); // Restore an in-flight drag; do not discard earlier local edits.
bool NeedsTwoStairs (double areaM2, const massingareas::Coefficients& coefficients);
bool Dirty (const Draft& draft);
bool Conflict (const Plan& plan, const Draft& draft);
void Reset (const Plan& plan, Draft& draft);
void Sync (const Plan& plan, Draft& draft);
// Pure targeted edits; the owner writes outside ImGui and checks building identity.
std::vector<hudmeta::Edit> Edits (const Plan& plan, const Draft& draft);
std::vector<hudmeta::Edit> Draw (const Plan& plan, Draft& draft, float scale,
                                 const massingareas::Coefficients& coefficients);
} // namespace geomsrv::archviz::buildingplan
#endif
