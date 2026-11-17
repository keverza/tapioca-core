#ifndef EVP_EDIT_EDITTERRAINBAKE_HPP
#define EVP_EDIT_EDITTERRAINBAKE_HPP

// The built terrain as an Archicad Mesh would be made from it (tapioca-dev #177): a boundary
// ring with a height per vertex and level lines inside it. Polylines, never loose points.
//
// Three ways of choosing the level lines:
//   Contours   horizontal cuts of the surface every `interval` metres;
//   Signature  what was modelled (the user, 2026-11-16: "a path actually only needs 2
//              contours"): the authored outlines (paths' paved edges, grading regions' rings)
//              draped on the surface, and the toe -- where the built surface leaves the
//              existing ground -- outside them; a few coarse contours of the ground the design
//              left alone. That ground keeps its own shape: its folds (every edge its two faces
//              meet at more than `creaseDegrees`) are lines, so a raised mesh point becomes the
//              lines through it, and a gentler peak or pit gets a line down its steepest edge.
//              The graded surface's folds only when `folds` asks; every fold when no design is
//              given.
//   Surface    the surface as it is (Mesh detail, EditTerrainDetail.hpp): its kept edges as lines,
//              unreduced, and every other vertex on a line along the surface's own edges -- so
//              the Mesh, triangulating the same points with those lines, is the same surface.
// Each Contours or Signature line is reduced to `tolerance`: a point is dropped while the line stays within that
// distance of the traced one. Where a line ends on the boundary, the boundary has a vertex.
//
// This is a plan only: pure geometry, no model read or write. The plan is what the preview
// draws and, unchanged, what the native Mesh is created from.
#include "Edit/EditTypes.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::edit {

enum class BakeMode { Contours, Signature, Surface };

struct BakeOptions {
    BakeMode mode = BakeMode::Contours;
    double interval = 0.0;      // metres; 0: a round one (about ten levels, four for Signature)
    double tolerance = 0.05;    // metres a reduced line may leave the traced one by
    double creaseDegrees = 4.0; // Signature with folds: the fold an edge needs to be a line
    bool folds = false;         // Signature: the surface's folds as well as the design's lines
    double shortest = 1.0;      // Signature: metres an open design line needs to be kept
    bool operator== (const BakeOptions&) const = default;
};

// Signature's input beside the surface: the design as it was modelled.
struct BakeDesign {
    std::vector<double> base;                // existing ground height per surface point; empty: no toe
    std::vector<std::vector<Vec3>> outlines; // authored closed rings; their plan is draped on the surface
    double ground = 0.01;                    // metres a point may move and still be existing ground
    // Surface: the surface's edges that must be lines (its kept lines), as point index pairs.
    std::vector<std::array<uint32_t, 2>> edges;
};

enum class BakeLineKind { Contour, Crease, Outline, Toe };

struct BakeLine {
    BakeLineKind kind = BakeLineKind::Contour;
    bool closed = false;      // a ring: the last point joins the first (not repeated here)
    std::vector<Vec3> points; // reduced, absolute heights
    size_t traced = 0;        // points before the reduction
};

struct BakePlan {
    bool ok = false;
    std::string error;              // why there is no plan
    std::vector<std::string> notes; // what the Mesh will not carry, said before it is made
    std::vector<Vec3> outline;      // counter-clockwise, distinct, absolute heights
    size_t outlineTraced = 0;
    std::vector<BakeLine> lines;
    size_t points = 0, traced = 0; // level-line points after and before the reduction
    size_t holes = 0;              // inner boundary loops, which the Mesh does not get
    double lo = 0.0, hi = 0.0, interval = 0.0;
};

// `points` xyz triples, `triangles` index triples (counter-clockwise from above).
BakePlan PlanTerrainBake (const std::vector<double>& points, const std::vector<uint32_t>& triangles,
                          const BakeOptions& options, const BakeDesign& design = {});

// The plan in the native create's own frame: heights measured from a base plane `skirtDepth`
// under the lowest point (`baseLevel`, absolute), a closed line's first point repeated at its end.
struct BakePayload {
    std::vector<double> outline, polyZ, ridgeCoords; // xy pairs; one z each; xyz triples
    std::vector<int32_t> ridgeCounts;
    double baseLevel = 0.0;
};
BakePayload TerrainBakePayload (const BakePlan& plan, double skirtDepth = 1.0);
// What the native create would refuse or misbuild, empty when the payload is sound.
std::vector<std::string> CheckBakePayload (const BakePayload& payload);

} // namespace geomsrv::edit
#endif
