#ifndef EVP_SUNSTUDY_SUNSTUDYPATCHSAMPLER_HPP
#define EVP_SUNSTUDY_SUNSTUDYPATCHSAMPLER_HPP

// SunStudy/SunStudyPatchSampler — the SurfacePatch domain, as the running
// engine's sample set.
//
// ⚠️ THIS IS A SECOND FRONT END, NOT A SECOND ENGINE. Only the
// geometry-to-samples stage differs:
//
//     snapshot triangles -> TriangleLegacy -> the existing sampler -> samples
//     snapshot triangles -> SurfacePatch builder -> clipped cells -> samples
//
// Everything downstream -- the occlusion accumulator, the traversal, the hours
// arithmetic, the atlas scatter -- consumes the same flat parallel arrays it
// always did. A fork any deeper than this would be two study engines, and the
// first defect fixed in one would live on in the other.
//
// ⚠️ THE TRIANGLE PATH STAYS. It is the oracle: patch mode deliberately MOVES the
// sample points (that is the whole point -- they stop depending on how Archicad
// tessellated the surface), so the two cannot be compared sample for sample.
// They can be compared on area, on the mean, and on convergence as the spacing
// falls, and that comparison is only possible while both exist.
//
// ⚠️ NO ACAPI, NO Diligent.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "SunStudy/SunStudyCache.hpp" // PatchKey
#include "SunStudy/SurfacePatch.hpp"

namespace evp::sunstudy {

// Which domain a study measures.
enum class SamplingDomain : uint8_t {
    // One grid per source TRIANGLE. The original, kept as the oracle.
    TriangleLegacy = 0,
    // One grid per coplanar edge-connected SURFACE. Sample positions do not
    // depend on the tessellation.
    SurfacePatch = 1,
};

// One patch's samples, as a CONTIGUOUS RANGE of the study's arrays.
//
// ⚠️ CONTIGUITY IS WHAT MAKES INCREMENTAL ANALYSIS A SUBSET RATHER THAN A
// REWRITE. The accumulator already takes a `SampleSet` of (pointer, count), so a
// patch's own span is a valid sample set with no copying, no scatter list and no
// second accumulator implementation -- recomputing one surface is the same code
// over a shorter array.
struct PatchSampleSpan {
    PatchKey key;
    // Index of the first sample and how many, into the flat arrays below.
    size_t first = 0;
    size_t count = 0;

    // The patch's own lattice, so the display can map a world point into it
    // without consulting the geometry again.
    uint32_t columns = 0;
    uint32_t rows = 0;
    double origin[3] = { 0.0, 0.0, 0.0 };
    double uAxis[3] = { 0.0, 0.0, 0.0 };
    double vAxis[3] = { 0.0, 0.0, 0.0 };
    double normal[3] = { 0.0, 0.0, 1.0 };

    // World AABB, kept so dirty classification needs no geometry.
    double boundsMin[3] = { 0.0, 0.0, 0.0 };
    double boundsMax[3] = { 0.0, 0.0, 0.0 };

    double area = 0.0;
};

// The study's sample set, in patch order.
struct PatchSampleGrid {
    // Flat and parallel, exactly as the triangle sampler produces.
    std::vector<double> positions; // xyz, lifted along the normal
    std::vector<double> normals;   // xyz unit
    std::vector<double> areas;     // m², clipped at the patch boundary

    // Per sample, its place in its own patch's lattice. ⚠️ CARRIED BECAUSE THE
    // ATLAS NEEDS IT: a patch tile is its lattice, and a sample without its cell
    // has no texel.
    std::vector<uint32_t> cellColumns;
    std::vector<uint32_t> cellRows;
    // Per sample, which span owns it. Redundant with the spans and kept anyway:
    // a scatter loop that had to binary-search the spans per sample is the kind
    // of thing that silently becomes the hot loop.
    std::vector<uint32_t> spanOf;

    std::vector<PatchSampleSpan> spans;

    size_t degenerateFaces = 0;
    // Patches whose lattice held no covered cell at all and that fell back to a
    // single centroid sample. ⚠️ REPORTED, NOT SILENT: coverage is a correctness
    // property and this is the count of surfaces that only just kept it.
    size_t centroidPatches = 0;
    bool valid = false;

    size_t Count () const
    {
        return areas.size ();
    }
    double TotalArea () const;
};

struct PatchSamplerOptions {
    double spacing = 1.0;
    // How far each sample is lifted along its normal, so a ray does not start on
    // the face it belongs to.
    double normalOffset = 0.01;
    size_t maxSamples = 4000000;
    SurfacePatchOptions patch;
};

// Build the patch-domain sample set for a snapshot's concatenated geometry.
//
// `elementOf` maps a group id to the owning element's GUID -- the identity the
// cache keys on. ⚠️ THE GUID IS NOT OPTIONAL: without it a patch has no identity
// that survives an edit, and the cache degenerates into the whole-study
// replacement it exists to replace.
PatchSampleGrid BuildPatchSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                      size_t faceCount, const uint32_t* groups,
                                      const std::vector<std::string>& elementOf,
                                      const PatchSamplerOptions& options = PatchSamplerOptions ());

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYPATCHSAMPLER_HPP
