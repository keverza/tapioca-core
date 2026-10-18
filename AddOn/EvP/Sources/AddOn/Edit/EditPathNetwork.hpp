#ifndef EVP_EDIT_EDITPATHNETWORK_HPP
#define EVP_EDIT_EDITPATHNETWORK_HPP

// Paths paved as one area. Each path's outline is a polygon; where paths cross or one
// runs into another they are united into T and + junctions. Every corner of that area is
// a kerb corner with its own radius, as a polyline's live corner is: a path's own corners
// (its ends', and its sharp bends' on either side) take the path's edge radius, a junction's
// (where two paths' sides meet) the larger of the two paths', and either can be set apart
// (Path::kerbs). An outside corner's rounding cuts it, a re-entrant one's fills it. Heights
// come from the paths -- the nearest one's profile and crossfall -- so the paved surface is
// a 3D mesh. Plan geometry in metres; nothing reads the model.
#include "Edit/EditPath.hpp"
#include "Edit/EditTypes.hpp"
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
    double maxRadius = 0;         // what it can take: most of the narrowest half width outside
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
    // Outlines counter-clockwise, holes clockwise, in plan with heights from the paths.
    std::vector<std::vector<Vec3>> rings;
    // The surface: triangles over `vertices`, counter-clockwise from above.
    std::vector<Vec3> vertices;
    std::vector<uint32_t> triangles;
    std::vector<PavedCorner> corners;
};

// `step`: metres between samples along each path. Paths that do not validate are left out.
// Without `surface`, only the outline and its corners (what grips need).
PavedArea Pave (const std::vector<Path>& paths, double step = 0.5, bool surface = true);

// A corner's grip: on its bisector at its arc's middle, or `minInset` metres into a sharp
// corner; the radius whose arc's middle is where `p` falls on the bisector (0 inside it).
Vec3 KerbGripAt (const PavedCorner& corner, double minInset);
double KerbRadiusFromGrip (const PavedCorner& corner, Vec3 p, double minInset);

// Sets one kerb corner's radius apart from its path's edge radius; `path` keeps it.
bool SetKerbRadius (Document& doc, Id path, const KerbCorner& corner, double radius, std::string& error);

// A path's surface at plan point `p`: the centre's elevation with the crossfall carried to
// the offset. `beyond` is how far `p` lies outside the path's width (negative inside).
bool SurfaceHeight (const Path& path, const AlignmentGeometry& geometry, Vec3 p, double& z, double& beyond);

} // namespace geomsrv::edit
#endif
