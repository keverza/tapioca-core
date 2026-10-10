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
constexpr double kStairWidth = 4.5, kStairDepth = 4.2, kSnapDistance = 0.5;
constexpr double kMinCore = 2.0, kMaxCore = 12.0;
constexpr double kCoreFacadeGap = 3.0, kMinUnitArea = 25.0, kMinUnitDepth = 3.3;
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
// Semantic locks: what a locked flat keeps through regeneration and Optimise.
enum Trait : uint8_t { kCorner = 1, kDualAspect = 2, kStraightFacade = 4 };
// Native quick scheme on the building frame, not the private optimiser or a compliance result.
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
    // Boundary pieces of a segment along its axis: on the counted outline (facade) or on
    // circulation (entrance). `side` is the outward direction in the frame: +x, +y, -x, -y.
    struct Edge {
        double a0, a1, length, across;
        uint8_t side;
    };
    std::vector<Edge> facade, access;
    std::vector<Point> triangles; // Cached hole-aware fill triangles, in world XY.
};
struct UnitSeed {
    uint32_t id = 0;
    size_t segment = 0;
    double along = 0;
    size_t type = 0;       // programme flat type
    double target = 0;     // that type's net target when chosen; maps the seed across programme edits
    bool locked = false;   // keeps type, size and `keep` through edits, Regenerate and Optimise
    uint8_t keep = 0;      // Trait bits a locked flat keeps
    double lo = 0, hi = 0; // Solved shared cuts in the segment's local axis.
};
struct UnitTraits {
    double gross = 0, net = 0, facade = 0, access = 0, depth = 0;
    uint8_t sides = 0;  // facade directions, bit per `PlanRegion::Edge::side`
    uint8_t traits = 0; // Trait bits it has
};
// Walking distance along the corridors (cells of one 0.3 m module) to the stairs.
struct Egress {
    double longest = 0;         // farthest corridor cell to its nearest stair, metres
    std::vector<Point> invalid; // cells beyond 25 m (dead end) or 40 m (two stairs), world XY
    size_t cells = 0;
};
struct QuickPlan {
    std::string signature, note;
    std::string outlineSignature;
    std::string solveNote;
    Point origin;
    double angle = 0;
    floorprogramme::Programme programme = floorprogramme::Default ();
    std::vector<PlanRegion> bars, corridors, bands, segments, units;
    std::vector<PlanRegion> unassigned; // floor no flat or circulation reaches
    std::vector<UnitSeed> seeds;
    std::vector<Core> cores;
    Egress egress;
    double score = 0;
    uint64_t alternative = 0;
    uint32_t nextId = 1;
    int stage = 6, selected = -1;
    bool adding = false, dragging = false;
    uintptr_t owner = 0;
    double dragOriginal = 0;
    double dragRequestedAlong = std::numeric_limits<double>::quiet_NaN ();
    Point dragOffset;
    std::vector<UnitSeed> dragSeeds;
    std::vector<PlanRegion> dragUnits;
    size_t newType = 1;
    bool ready = false;
    uint64_t revision = 0;
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
    bool known = false, placing = false;
    bool moving = false, dragging = false;
    uintptr_t dragOwner = 0; // Only the initiating ImGui context consumes the held-pointer gesture.
    Point dragOffset, dragOriginal;
    int selected = -1; // -1: append a new stair; otherwise move the selected marker.
    int story = (std::numeric_limits<int>::min) ();
    std::vector<Core> original, cores;
    Core newCore; // size of the next placed stair
    bool changed = false;
    bool originalMixed = false;
    bool moveCores = true; // Optimise may move proposed cores
    std::vector<std::string> guids;
    std::vector<std::string> fingerprints;
    floorprogramme::Programme programme = floorprogramme::Default (); // the Massing programme, HUD-session
    // The floor designs (stairs aside: `cores`), and as saved; Save writes them with the stairs.
    floorscheme::edit::Designs designs, originalDesigns;
    bool exportRequested = false;        // Export plan pressed; the owner builds and writes the file
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
// World corners of a core turned to the building frame.
std::vector<Point> Corners (const Core& core, double angle = 0);
bool Fits (const Floor& floor, const Core& core, double angle = 0);
// Project layout rule, separate from footprint containment: flush to a wall or >= 3 m clear.
bool CoreAllowed (const Floor& floor, const Core& core, double angle = 0);
Point Snap (const Floor& floor, const Core& core, double angle = 0);
std::string QuickSignature (const Floor& floor, const std::vector<Core>& stairs,
                            const floorprogramme::Programme* programme = nullptr);
// `angle` is the building frame; NaN derives it from this floor's longest edge.
QuickPlan GenerateQuick (const Floor& floor, const std::vector<Core>& stairs,
                         const floorprogramme::Programme& programme = floorprogramme::Default (),
                         double angle = std::numeric_limits<double>::quiet_NaN ());
Point UnitCenter (const QuickPlan& plan, const UnitSeed& seed);
bool AddUnit (QuickPlan& plan, Point point);
bool MoveUnit (QuickPlan& plan, size_t seed, Point point);
bool RemoveUnit (QuickPlan& plan, size_t seed);
double TargetArea (const QuickPlan& plan, const UnitSeed& seed); // net m2
double Rooms (const QuickPlan& plan, const UnitSeed& seed);
std::string TypeName (const QuickPlan& plan, const UnitSeed& seed);
uint32_t UnitColour (double rooms);
uint32_t AreaColour (double netArea);
double UnitArea (const PlanRegion& unit); // gross polygon area
UnitTraits Traits (const QuickPlan& plan, const UnitSeed& seed);
int UnitAt (const QuickPlan& plan, Point point);
bool SetUnitType (QuickPlan& plan, size_t seed, size_t type);
// Locking keeps the flat's type and net area and, by default, the traits it has now.
bool SetUnitLocked (QuickPlan& plan, size_t seed, bool locked);
bool SetUnitKeep (QuickPlan& plan, size_t seed, uint8_t keep);
void BuildAreaProfile (const QuickPlan& plan, PlanRegion& segment);
double AreaBefore (const PlanRegion& segment, double along);
double AlongAtArea (const PlanRegion& segment, double area);
// Solve the shared cuts of `seeds` for net targets; false (seeds untouched) when infeasible.
bool SolveCuts (const QuickPlan& plan, std::vector<UnitSeed>& seeds, std::string& note, int anchor = -1,
                bool exactTarget = false, bool dragging = false);
bool RelaxUnits (QuickPlan& plan, int anchor = -1, bool exactTarget = false, bool dragging = false);
// Lower is better: programme ranges and mix, corners, entrances, locks, empty floor, egress.
double Score (const QuickPlan& plan, const std::vector<UnitSeed>& seeds);
// Programme fill: flat count and types per segment from the shares, larger types at corners.
void Fill (QuickPlan& plan);
// Bounded local search on unlocked flats (type, count, order); deterministic.
bool OptimiseUnits (QuickPlan& plan, size_t budget = 400);
Egress AnalyseEgress (const QuickPlan& plan);
bool ChangeUnitTarget (const Plan& plan, Draft& draft, const Floor& floor, size_t seed, size_t type, bool locked);
bool TransferUnits (const QuickPlan& old, QuickPlan& next, int anchor = -1);
// Carry the locked flats of `old` into the fresh fill `next`, replacing the flats they land on.
bool KeepLocked (const QuickPlan& old, QuickPlan& next);
void PartitionUnits (QuickPlan& plan);
void RebuildUnits (QuickPlan& plan);
void CancelUnits (QuickPlan& plan);
void UseProgramme (Draft& draft, const floorprogramme::Programme& programme);
QuickPlan& QuickFor (const Plan& plan, Draft& draft, const Floor& floor);
void MakeUnique (const Plan& plan, Draft& draft, const Floor& floor);
void ResetQuick (const Plan& plan, Draft& draft, const Floor& floor);
// Fresh programme fill around the locked flats, then Optimise without moving cores.
bool Regenerate (const Plan& plan, Draft& draft, const Floor& floor);
// Improve the floor's design; with `draft.moveCores`, try bounded core moves valid on every floor.
bool Optimise (const Plan& plan, Draft& draft, const Floor& floor);
void PreviewLayers (const Plan& plan, Draft& draft, overlaylayers::Layer& stairs, overlaylayers::Layer& units,
                    bool includeUnits = true);
bool Place (const Floor& floor, Draft& draft, Point point, double angle = 0);
bool BeginDrag (Draft& draft, Point mouse, uintptr_t owner = 0, double angle = 0);
bool Drag (const Floor& floor, Draft& draft, Point mouse, double angle = 0);
void EndDrag (Draft& draft);
void Cancel (Draft& draft); // Restore an in-flight drag; do not discard earlier local edits.
bool NeedsTwoStairs (double areaM2, const massingareas::Coefficients& coefficients);
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
std::vector<hudmeta::Edit> Draw (const Plan& plan, Draft& draft, float scale,
                                 const massingareas::Coefficients& coefficients);
} // namespace geomsrv::archviz::buildingplan
#endif
