#ifndef EVP_EDIT_EDITTERRAINBAKESURFACE_HPP
#define EVP_EDIT_EDITTERRAINBAKESURFACE_HPP

// tapioca-dev #177: the surface an Archicad Mesh made from a bake plan has -- its outline as the
// boundary, every level line a breakline and every level point a vertex -- triangulated by the
// Rust TIN (constrained Delaunay, no refinement), so the editor can show the light Mesh it will
// create (the user, 2026-11-20: "topography preview and commit should have contour
// simplification"). Archicad triangulates the Mesh it is given on its own: the two surfaces share
// every point and every line, but a triangle between them may be split the other way.
//
// Derived geometry only: no authored identity, no document read. Rust-only, like the build.
#include "Edit/EditTerrainBake.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::edit {

struct BakeSurface {
    std::vector<double> points; // xyz
    std::vector<uint32_t> triangles;
};

// False with the reason when the plan has no outline or the triangulation refuses (a level line
// crossing another or leaving the outline, for one). `surface` is unchanged then.
bool BakePlanSurface (const BakePlan& plan, BakeSurface& surface, std::string& error);

} // namespace geomsrv::edit
#endif
