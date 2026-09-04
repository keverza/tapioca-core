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
