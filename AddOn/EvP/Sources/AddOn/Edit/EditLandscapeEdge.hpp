#ifndef EVP_EDIT_EDITLANDSCAPEEDGE_HPP
#define EVP_EDIT_EDITLANDSCAPEEDGE_HPP

// Curbs, retaining walls and stairs (EdgeSpec): where the designed ground steps instead of
// sloping. This is the offline half -- the rules an edge must satisfy and the checks of a
// terrain against it. Splitting the terrain along an edge belongs to EditTerrainTriangulation,
// and writing walls or reading Archicad stairs to the native commands; neither is done here.
#include "Edit/EditLandscape.hpp"
#include <functional>

namespace tapioca::geometry {

constexpr double kMinWallThickness = 0.05;  // a retaining wall is never thinner than 50 mm
constexpr double kMinStairClearance = 0.05; // the default gap under a stair

// The project's thresholds. A step up to `maxCurbHeight` is a curb; ground steeper than
// `maxGrade` percent is a jump an edge has to carry.
struct EdgeLimits {
    double maxCurbHeight = 0.30;
    double maxGrade = 100.0;
};

// The terrain's height at a plan point; false where there is none.
using HeightAt = std::function<bool (double x, double y, double& z)>;

// The edge's baseline end points in plan, at Z 0. Refuses a missing path or one whose control
// points are not on one straight line (EdgeBaselineNotStraight).
bool EdgeBaseline (const geomsrv::edit::Document& doc, const EdgeSpec& edge, Vec3& start, Vec3& end,
                   std::string& error);
// The edge's own rules: a curb no higher than the limit (CurbTooHigh), a wall at least
// kMinWallThickness thick with its top above its foot, a stair whose steps climb exactly from
// its foot to its top and fit on the baseline. Errors start with their code.
bool ValidateEdge (const geomsrv::edit::Document& doc, const EdgeSpec& edge, const EdgeLimits& limits,
                   std::string& error);

// One document edit each; the edge is validated first and nothing changes on a refusal.
bool AddLandscapeEdge (geomsrv::edit::Document& doc, EdgeSpec edge, const EdgeLimits& limits, Id& id,
                       std::string& error);
bool UpdateLandscapeEdge (geomsrv::edit::Document& doc, const EdgeSpec& edge, const EdgeLimits& limits,
                          std::string& error);
bool RemoveLandscapeEdge (geomsrv::edit::Document& doc, Id edge, std::string& error);
const EdgeSpec* FindLandscapeEdge (const geomsrv::edit::Document& doc, Id edge);

// ---- stairs ---------------------------------------------------------------------------

// A stair's envelope: its flight climbs from the baseline's start, `steps` treads of `run`,
// then a level landing; `width` across, centred on the baseline.
struct StairEnvelope {
    Vec3 start, along, left; // plan foot, unit direction up the flight, unit left
    double width = 0.0, run = 0.0, rise = 0.0, landing = 0.0, bottomZ = 0.0, topZ = 0.0, clearance = 0.0;
    int steps = 0;
    double Length () const
    {
        return steps * run + landing;
    }
};
bool BuildStairEnvelope (const geomsrv::edit::Document& doc, const EdgeSpec& edge, StairEnvelope& out,
                         std::string& error);
// The underside above a plan point inside the footprint: one rise below the tread over it
// (the landing's level at the top). False outside the footprint.
bool StairUnderside (const StairEnvelope& stair, Vec3 p, double& z);
// The terrain under a stair, cut only: never above the underside less the clearance, and never
// raised towards it. Outside the footprint the terrain stays as it is.
double StairCutHeight (const StairEnvelope& stair, Vec3 p, double terrainZ);

// The terrain sampled every `step` metres across the footprint: where it rises into the
// clearance (cuts, the deepest `maxCut`) and the widest air gap left under the stair.
struct StairClearance {
    size_t samples = 0;
    std::vector<Vec3> cuts; // plan point, Z the terrain there
    double maxCut = 0.0, maxGap = 0.0;
};
bool CheckStairClearance (const StairEnvelope& stair, const HeightAt& height, double step, StairClearance& out,
                          std::string& error);

// ---- curbs and walls ------------------------------------------------------------------

// The ground on both faces of a curb or wall, sampled every `step` metres along it: the faces
// are `width`/2 off the baseline, the high one on `highSide`.
struct EdgeFaces {
    size_t samples = 0;
    double highMin = 0.0, highMax = 0.0, lowMin = 0.0, lowMax = 0.0;
};
bool SampleEdgeFaces (const geomsrv::edit::Document& doc, const EdgeSpec& edge, const HeightAt& height, double step,
                      EdgeFaces& out, std::string& error);
// A curb: each side's ground meets it at its level, within `tolerance`. The error names the
// side and how far off it is (CurbLevelMismatch).
bool CheckCurbLevels (const geomsrv::edit::Document& doc, const EdgeSpec& edge, const HeightAt& height, double step,
                      double tolerance, std::string& error);
// A retaining wall covers the ground crossing it: the slope may run through the wall's body,
// but the high-side ground stays at or below its top (else it is buried) and the low-side
// ground at or above its foot (else it is undermined). WallCoverageIncomplete otherwise.
bool CheckWallCoverage (const geomsrv::edit::Document& doc, const EdgeSpec& edge, const HeightAt& height, double step,
                        std::string& error);

// ---- steep jumps ----------------------------------------------------------------------

// A run of `line` where the ground is steeper than the limit, and the edge that would carry
// it: a curb for a small drop, a stair across a walkway, a retaining wall otherwise.
// `existing` is an edge already on it. Only a suggestion: nothing is inserted.
struct SteepJump {
    Vec3 from, to; // plan, Z the ground
    double drop = 0.0;
    EdgeKind suggestion = EdgeKind::Curb;
    Id existing = kNoId;
};
std::vector<SteepJump> FindSteepJumps (const geomsrv::edit::Document& doc, const std::vector<Vec3>& line,
                                       const HeightAt& height, double step, const EdgeLimits& limits, bool walkway);

} // namespace tapioca::geometry
#endif
