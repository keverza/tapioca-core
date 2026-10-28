#ifndef EVP_EDIT_EDITPATHNETWORK_HPP
#define EVP_EDIT_EDITPATHNETWORK_HPP

// Paths paved as one area. Each path's outline is a polygon; where paths cross or one
// runs into another they are united into T and + junctions. Every corner of that area is
// a kerb corner with its own radius, as a polyline's live corner is: a path's own corners
// (its ends', and its sharp bends' on either side) take the path's edge radius, a junction's
// (where two paths' sides meet) the larger of the two paths', and either can be set apart
// (Path::kerbs). An outside corner's rounding cuts it, a re-entrant one's fills it. Heights
// sample the paths' profiles and crossfalls into one shared-edge 3D mesh. Contours
// evaluate that mesh rather than separate strips. Plan metres; nothing reads the model.
#include "Edit/EditPath.hpp"
#include "Edit/EditPathOutlineCache.hpp"
#include "Edit/EditTypes.hpp"
#include "Geometry/Fillet2D.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::edit {

struct Document;

// A corner of the paved area, as found: which it is (`key`, kept on path `path` -- for a
// junction the one with the lower id) and the radius it is rounded by (`key.radius`).
struct PavedCorner {
    Id path = kNoId;
    KerbCorner key;
    Vec3 at;                      // the sharp corner, at the surface's height
    Vec3 bisector;                // plan unit, into the wedge its rounding cuts (outside) or fills
    double half = 0.0;            // half that wedge's angle
    bool convex = true;           // an outside corner
    double maxRadius = 0;         // feasible with the current neighbours' fitted radii
    double radiusLimit = 0;       // hardscape policy cap; a new request may rebalance neighbours
    double requestedRadius = 0.0; // stored intent, retried when the surrounding paths change
    bool Limited () const
    {
        return requestedRadius > key.radius + 1.0e-9;
    }
    bool Involves (Id id) const
    {
        return path == id || key.other == id;
    }
};

struct PavedArea {
    fillet2d::Region boundary; // analytic line/arc construction, before mesh sampling
    std::string warning;       // a refused rounding keeps the sharp region, never silent retopology
    // Outlines counter-clockwise, holes clockwise, in plan with heights from the paths.
    std::vector<std::vector<Vec3>> rings;
    // The surface: triangles over `vertices`, counter-clockwise from above.
    std::vector<Vec3> vertices;
    std::vector<uint32_t> triangles;
    std::vector<PavedCorner> corners;
};

// `step`: target sample/mesh-edge spacing, bounded at 250,000 triangles. Authored
// profile/section features are inserted at both preview and resting quality.
// Footprints are sampled at no coarser than 0.5 m, independently of surface quality.
// Paths that do not validate are left out.
// Without `surface`, only the outline and its corners (what grips need). `outlines` (optional)
// spares the paths' tessellation when another call of the frame already did it; the result is
// identical with or without it.
PavedArea Pave (const std::vector<Path>& paths, double step = 0.5, bool surface = true,
                PathOutlineCache* outlines = nullptr);

// The step a preview of `paths` is paved at while a gesture edits them: the coarsest that keeps
// the surface near `triangles` triangles, never finer than `finest` nor coarser than `coarsest`.
// A paved surface has about kPreviewDensity triangles per square metre at a 1 m step, falling
// with the step squared, and its area is close to the paths' plan length times width, so the
// preview costs about the same however large the network grows. The step is rounded up to
// whole half metres, so it holds still while a drag reshapes a path. Authored features are
// inserted at any step (Pave), so a coarse preview keeps every break, range and corner.
constexpr double kPreviewDensity = 16.0;
double PreviewStep (const std::vector<Path>& paths, size_t triangles = 4000, double finest = 0.5,
                    double coarsest = 8.0);

struct StationSpan {
    double from = 0, to = 0;
};
// Runs where a path's own width/fall can be measured without crossing a junction.
std::vector<StationSpan> PathAnnotationSpans (const std::vector<Path>& paths, Id path, double from, double to);
// Height on the actual unified piecewise-linear surface, not an independent strip.
bool PavedHeight (const PavedArea& area, Vec3 point, double& z);
// Points along a path's own outline (its edges, a range's) lifted onto that one surface where it
// covers them. An outline edge carries its cross-section's height, and on the inside of a bend two
// sections fold onto one plan point with two heights; a line drawn from them leaves the surface
// it outlines (a 0.2 m step at a 20 % grade's inner corner). Points it does not cover keep theirs.
void OntoPavedSurface (const PavedArea& area, std::vector<Vec3>& points);

// A corner's grip: on its bisector at its arc's middle, or `minInset` metres into a sharp
// corner; the radius whose arc's middle is where `p` falls on the bisector (0 inside it).
Vec3 KerbGripAt (const PavedCorner& corner, double minInset);
double KerbRadiusFromGrip (const PavedCorner& corner, Vec3 p, double minInset);

// Sets one kerb corner's radius apart from its path's edge radius; `path` keeps it.
bool SetKerbRadius (Document& doc, Id path, const KerbCorner& corner, double radius, std::string& error);

// A path's surface at plan point `p`: the centre's elevation with the crossfall carried to
// the offset. `beyond` includes width and end caps (negative inside).
bool SurfaceHeight (const Path& path, const AlignmentGeometry& geometry, Vec3 p, double& z, double& beyond);

} // namespace geomsrv::edit
#endif
