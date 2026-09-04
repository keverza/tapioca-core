#ifndef EVP_SUNSTUDY_SUNSTUDYSAMPLER_HPP
#define EVP_SUNSTUDY_SUNSTUDYSAMPLER_HPP

// SunStudy/SunStudySampler — turning a triangle soup into the points the study
// actually measures.
//
// The question this answers is "where do we ask?", and it is separate from
// "what is the answer" on purpose: a sample set is a value, so the same points
// can be handed to the ray-cast core, to a raster preview and to a viewer's
// display atlas, and all three then measure THE SAME PLACES. Two consumers that
// each invent their own points cannot be diffed, and a study nobody can diff is
// a study nobody can trust.
//
// ⚠️ THIS IS A GRID OVER EACH FACE, NOT A SUBDIVISION OF IT. The older approach
// split triangles until every edge was short enough and then measured the
// VERTICES, which ties sample density to the source topology: a long thin
// triangle and a fat one get wildly different coverage, shared vertices have to
// be welded away afterwards, and a vertex sits ON an edge where the surface
// normal is ambiguous. Rasterising cell CENTRES into each face instead gives a
// density that depends only on the requested spacing, needs no weld, and puts
// every sample strictly inside one face with one unambiguous normal.
//
// ⚠️ EVERY NON-DEGENERATE FACE EMITS AT LEAST ONE SAMPLE. A face smaller than
// the grid contains no cell centre, and dropping it would silently delete small
// elements from the study -- mullions, treads, railings -- while the totals
// still looked plausible. Such a face emits its centroid. Coverage is a
// correctness property here, not a quality setting.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "SunStudy/SunStudyRaster.hpp"

namespace evp::sunstudy {

// Where one face's grid sits, and in what frame.
//
// ⚠️ THIS IS WHAT LETS A POINT-SAMPLED STUDY BE DRAWN ON CONTINUOUS GEOMETRY.
// The samples are cell centres, not vertices, so a renderer cannot simply
// interpolate them: given an arbitrary point on a face it has to work out WHICH
// cell that point falls in. Recording the face's own frame and its cell origin
// makes that an affine map instead of a search, and it is the same map the
// atlas uses to hand every vertex a texture coordinate.
struct FaceLayout {
    double origin[3] = { 0.0, 0.0, 0.0 }; // the face's first corner
    double uAxis[3] = { 0.0, 0.0, 0.0 };  // unit, along the first edge
    double vAxis[3] = { 0.0, 0.0, 0.0 };  // unit, in-plane, perpendicular to uAxis

    // The integer cell the face's grid starts at, in each axis. Cell (i, j)
    // spans [i * spacing, (i + 1) * spacing) along uAxis, and likewise for j.
    long uStart = 0;
    long vStart = 0;

    // Cells spanned. ⚠️ NOT THE SAMPLE COUNT: cells outside the triangle emit
    // no sample, so a face's tile is always at least as large as its sample
    // count and usually larger. The atlas allocates by CELLS, because that is
    // what an arbitrary point maps into.
    uint32_t columns = 0;
    uint32_t rows = 0;

    // False for a face that was skipped, or that fell below the grid and emitted
    // only its centroid. Such a face has no cell lattice to map into.
    bool gridded = false;
};

// The measured points, in the layout every consumer wants: flat, interleaved,
// and parallel across all five arrays.
struct SampleGrid {
    std::vector<double> positions; // xyz, already lifted off the surface
    std::vector<double> normals;   // xyz unit face normals
    std::vector<double> areas;     // m^2 carried by each sample; sums to the mesh area
    std::vector<uint32_t> faces;   // source triangle index
    std::vector<uint32_t> groups;  // caller's per-triangle group id, verbatim

    size_t Count () const
    {
        return faces.size ();
    }

    // One entry per SOURCE face, in source order, so `layouts[faces[i]]` is the
    // layout the sample `i` came from. Present only when asked for: it costs a
    // fixed amount per face and only a consumer that draws the result needs it.
    std::vector<FaceLayout> layouts;

    // Texel of each sample within its own face's tile, as (column, row) offsets
    // from the layout's cell origin. Parallel to `faces`, and empty unless
    // layouts were requested.
    std::vector<uint32_t> cellColumns;
    std::vector<uint32_t> cellRows;

    // Faces skipped as degenerate. ⚠️ NOT AN ERROR AND NOT A WARNING TO HIDE:
    // exported Archicad meshes routinely carry zero-area slivers, and tracing
    // from one produces a normal built out of noise.
    size_t degenerateFaces = 0;

    // Faces that fell below the grid and emitted a centroid instead.
    size_t undersizedFaces = 0;

    // False when the requested spacing would have produced more samples than
    // `maxSamples`. ⚠️ REFUSAL, NOT TRUNCATION -- a truncated sample set is a
    // study of part of the model reported as a study of the model.
    bool valid = false;
};

struct SamplerOptions {
    // Target spacing in metres. The realised spacing is exactly this; it is the
    // COUNT that varies with face size, never the pitch.
    double spacing = 1.0;

    // How far each sample is lifted along its normal. Keeps a ray from starting
    // exactly on the surface it belongs to; works with, not instead of, the
    // traversal's `tmin`.
    double normalOffset = 0.01;

    // Jitter as a fraction of a cell, 0 for exact centres. ⚠️ DERIVED FROM THE
    // FACE AND CELL INDICES, NEVER FROM A RUNNING RANDOM STATE, so the same
    // mesh and spacing always give the same points -- on any thread, in any
    // order, on any machine. A study that cannot be reproduced cannot be
    // regression-tested, and jitter exists to break grid aliasing against
    // regular architecture, not to add entropy.
    double jitter = 0.0;

    size_t maxSamples = 4000000;

    // Fill `layouts`, `cellColumns` and `cellRows`. Off by default: only a
    // consumer that draws the study needs them.
    bool wantLayouts = false;
};

// `vertices` is xyz-interleaved; `triangles` is 3 indices per face; `groups` is
// one id per face or empty. Faces whose normal cannot be computed are skipped.
SampleGrid BuildSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                            const uint32_t* groups, const SamplerOptions& options);

// Exposed for testing, and because both are useful on their own.
Vec3 TriangleNormal (const Vec3& a, const Vec3& b, const Vec3& c, double* areaOut = nullptr);

// The deterministic per-cell offset described under `jitter`. Returns two values
// in [-0.5, 0.5], scaled by the caller.
void CellJitter (uint32_t face, uint32_t u, uint32_t v, double out[2]);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYSAMPLER_HPP
