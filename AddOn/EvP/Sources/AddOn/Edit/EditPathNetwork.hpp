#ifndef EVP_EDIT_EDITPATHNETWORK_HPP
#define EVP_EDIT_EDITPATHNETWORK_HPP

// Paths paved as one area. Each path's outline is a polygon; where paths cross or one
// runs into another they are united into T and + junctions, and a path's edge radius (a
// kerb radius) rounds its edges' outside corners and, at junctions, the re-entrant corners
// where edges meet. Heights come from the paths -- the nearest one's profile and crossfall
// -- so the paved surface is a 3D mesh. Plan geometry in metres; nothing reads the model.
#include "Edit/EditPath.hpp"
#include <cstdint>
#include <vector>

namespace geomsrv::edit {

struct PavedArea {
    // Outlines counter-clockwise, holes clockwise, in plan with heights from the paths.
    std::vector<std::vector<Vec3>> rings;
    // The surface: triangles over `vertices`, counter-clockwise from above.
    std::vector<Vec3> vertices;
    std::vector<uint32_t> triangles;
};

// `step`: metres between samples along each path. Paths that do not validate are left out.
PavedArea Pave (const std::vector<Path>& paths, double step = 0.5);

// A path's surface at plan point `p`: the centre's elevation with the crossfall carried to
// the offset. `beyond` is how far `p` lies outside the path's width (negative inside).
bool SurfaceHeight (const Path& path, const AlignmentGeometry& geometry, Vec3 p, double& z, double& beyond);

} // namespace geomsrv::edit
#endif
