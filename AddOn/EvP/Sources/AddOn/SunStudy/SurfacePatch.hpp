#ifndef EVP_SUNSTUDY_SURFACEPATCH_HPP
#define EVP_SUNSTUDY_SURFACEPATCH_HPP

// SunStudy/SurfacePatch — the analysis domain, above the renderer's triangles.
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS
//
// Today the chain is:
//
//     Archicad surface -> triangulated extraction -> sampler face == TRIANGLE
//                      -> atlas tile == TRIANGLE
//
// so a planar slab that Archicad happens to hand over as two triangles is
// treated as two analysis domains, with two grids, two lattices and a diagonal
// seam straight down the middle of a surface that has no seam in it. The
// renderer is showing exactly what the sampler defines; the fault is the
// definition, and no shader trick can hide it honestly.
//
// A SurfacePatch is the missing level: the coplanar, edge-connected run of
// source triangles that ONE BIM surface was tessellated into. Grid it once, in
// its own plane, and the cells are a regular rectangular lattice whose size is
// the spacing the user asked for -- whatever the triangulation underneath.
//
// ⚠️ THE TRIANGLES ARE NOT REPLACED, THEY ARE LAYERED OVER. The split that makes
// this clean is:
//
//     source triangles -> physical geometry, the occlusion BVH, the winding
//                         proof, extraction
//     surface patches  -> the sampling domain
//     grid cells       -> the measurements
//     atlas            -> the display
//
// which is four jobs that source triangles were doing alone.
//
// ⚠️ NO ACAPI, NO Diligent, NO RENDER VERTEX. Adjacency is built from the SOURCE
// triangle list, and that is a decision rather than a convenience: the render
// mesh is welded by (source vertex, normal), and this repository has already
// paid once for reasoning about faces through welded vertices. Source topology
// is the only thing that says which triangles were one surface.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "SunStudy/SunStudyRaster.hpp" // Vec3

namespace evp::sunstudy {

// One coplanar, edge-connected run of source triangles within one element.
struct SurfacePatch {
    // The caller's per-triangle group id -- an element index. ⚠️ PATCHES NEVER
    // CROSS IT. Two elements whose faces happen to be flush would otherwise
    // merge into one domain owned by neither, and every question the study is
    // later asked ("which element is in shade?") would have no answer.
    uint32_t group = 0;

    // Source triangle indices, ascending. Never empty.
    std::vector<uint32_t> triangles;

    // The plane, and the frame the grid is laid out in.
    double normal[3] = { 0.0, 0.0, 1.0 };
    // ⚠️ ONE ORIGIN AND ONE PAIR OF AXES FOR THE WHOLE PATCH -- this IS the
    // semantic change. Per-triangle frames are what give neighbouring triangles
    // different lattices and put a seam through a flat wall.
    double origin[3] = { 0.0, 0.0, 0.0 };
    double uAxis[3] = { 1.0, 0.0, 0.0 };
    double vAxis[3] = { 0.0, 1.0, 0.0 };

    // The patch's extent in its own frame, from `origin`. Both are >= 0 because
    // the origin is placed at the minimum corner.
    double uExtent = 0.0;
    double vExtent = 0.0;

    // Summed area of the source triangles, m². ⚠️ THE TRIANGLES' AREA, NOT THE
    // BOUNDING RECTANGLE'S: a patch is not necessarily convex or even simply
    // connected, and the difference is what a grid has to clip against.
    double area = 0.0;
};

struct SurfacePatchOptions {
    // How far two triangle normals may point apart and still be one surface.
    // ⚠️ SMALL ON PURPOSE. A tessellated cylinder is a run of nearly-coplanar
    // strips, and merging it into one flat patch would grid a curved surface as
    // if it were a plane. One degree keeps a genuinely flat wall together and a
    // curve apart.
    double coplanarDegrees = 1.0;

    // How far a neighbour's centroid may sit off this triangle's plane, metres.
    // Two parallel faces of a thin slab have identical normals up to sign and
    // must not merge; this is what separates them.
    double planeToleranceM = 1e-3;
};

// Group `faceCount` triangles into patches. `vertices` is xyz-interleaved,
// `triangles` is 3 indices per face, `groups` is one id per face or null.
//
// ⚠️ DEGENERATE TRIANGLES ARE DROPPED, NOT GROUPED. A zero-area sliver has a
// normal built out of noise, and letting one join a patch would rotate that
// patch's plane by whatever the noise happened to be.
std::vector<SurfacePatch> BuildSurfacePatches (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                               size_t faceCount, const uint32_t* groups,
                                               const SurfacePatchOptions& options = SurfacePatchOptions ());

// Where one cell of a patch's lattice sits.
struct PatchCell {
    uint32_t column = 0;
    uint32_t row = 0;
    // Cell centre in world space, already the point a study would measure.
    double centre[3] = { 0.0, 0.0, 0.0 };
    // ⚠️ THE CELL'S OWN COVERED AREA, WHICH IS spacing² ONLY IN THE INTERIOR. A
    // cell the patch boundary cuts through carries exactly the area the patch
    // covers inside it, and reporting spacing² for it would over-weight every
    // edge of every surface in the model -- which on a facade of mullions is
    // most of it, and which makes an area-weighted mean quietly wrong.
    double area = 0.0;
    // False when the patch boundary crosses this cell. A clipped cell's `centre`
    // is the AREA-WEIGHTED CENTROID of the covered part, not the middle of the
    // square: the middle of a square that is nine-tenths outside the surface is
    // a point in mid-air, and measuring sunlight there measures nothing.
    bool interior = false;
};

// The regular lattice over one patch, and which of its cells the patch covers.
//
// ⚠️ THE LATTICE IS THE PATCH'S, NOT THE TRIANGLES'. Every interior cell is
// exactly `spacing` x `spacing`, in one frame, for the whole surface -- which is
// the entire point of the patch level and the thing a per-triangle grid cannot
// do.
//
// `columns` and `rows` are the lattice's extent; `cells` holds only those a
// sample belongs in.
struct PatchGrid {
    uint32_t columns = 0;
    uint32_t rows = 0;
    std::vector<PatchCell> cells;
    // ⚠️ A NON-DEGENERATE PATCH ALWAYS PRODUCES AT LEAST ONE CELL, even one
    // smaller than the spacing -- it emits its centroid. Coverage is a
    // correctness property: dropping sub-grid surfaces deletes mullions and
    // treads from a study while every total still looks plausible.
    bool fromCentroid = false;
};

// Rasterise `patch` at `spacing` metres. `vertices` and `triangles` are the same
// arrays the patch was built from.
//
// ⚠️ EACH CELL IS CLIPPED AGAINST THE PATCH'S OWN TRIANGLES AND THE PIECES ARE
// SUMMED -- there is no union polygon anywhere in this. The source triangles
// PARTITION the original surface, so their clipped areas add up with no
// double-counting, and the sum is the covered area exactly. Building a union
// boundary first would need a polygon boolean, would need tolerance tuning at
// every shared edge, and would buy nothing: this handles concavity, holes and
// arbitrary triangulation as a consequence of what it already is.
//
// ⚠️ THE INVARIANT THAT MAKES IT CHECKABLE: for any spacing,
//     sum(cell.area) == sum(area of the patch's source triangles)
// to within rounding. A clipper that drops, double-counts or mis-orients a piece
// breaks it immediately, and no picture would have shown any of those.
PatchGrid BuildPatchGrid (const SurfacePatch& patch, const double* vertices, const uint32_t* triangles, double spacing);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SURFACEPATCH_HPP
