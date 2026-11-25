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

#include "SunStudy/SunStudyLimits.hpp" // kSamplerMaxSamples
#include "SunStudy/SunStudyRaster.hpp" // Vec3

namespace evp::sunstudy {

// Which domain a study measures.
enum class SamplingDomain : uint8_t {
    // One grid per source TRIANGLE. The original, kept as the oracle.
    TriangleLegacy = 0,
    // One grid per coplanar edge-connected SURFACE. Sample positions do not
    // depend on the tessellation.
    SurfacePatch = 1,
};

// A patch's identity ACROSS MODEL EDITS.
//
// ⚠️ NOT AN INDEX, AND NOT A TRIANGLE. An index moves when an unrelated element
// is added; a triangle is created and destroyed by any re-tessellation, so a
// triangle-keyed cache would invalidate itself on edits that changed nothing
// about the surface. The key is what the surface IS -- which element owns it,
// and where its plane sits -- so a patch of an untouched element keeps its
// identity through any change elsewhere in the model.
struct PatchKey {
    // The owning element. ⚠️ THE GUID, NOT THE SNAPSHOT'S MESH INDEX: that index
    // is a position in an array that changes whenever anything is added.
    std::string element;
    // The patch's plane and position, quantised. Two extractions of an unchanged
    // surface agree on it; a surface that moved does not.
    uint64_t plane = 0;

    // Distinct disconnected patches can share the same quantised plane/origin.
    // Their occurrence within that element's ordered patch set keeps tiles
    // separate; it is stable across edits/reorders of OTHER elements.
    uint32_t occurrence = 0;

    bool operator< (const PatchKey& other) const
    {
        if (element != other.element)
            return element < other.element;
        if (plane != other.plane)
            return plane < other.plane;
        return occurrence < other.occurrence;
    }
    bool operator== (const PatchKey& other) const
    {
        return element == other.element && plane == other.plane && occurrence == other.occurrence;
    }
};

// ⚠️ THE QUANTUM IS A DECISION, NOT A DETAIL. Too fine and floating-point noise
// between two extractions of the same wall gives it two identities, so the cache
// re-analyses a building that did not change. Too coarse and a wall nudged by
// less than the quantum keeps the identity -- and therefore the sunlight -- of
// where it used to be. A millimetre is far below anything a person models and
// far above extraction noise.
constexpr double kPatchKeyQuantumM = 1.0e-3;

// How triangles group into one surface (the Rust patch builder's options).
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

    // Per SOURCE TRIANGLE, the span that owns it, or `kNoPatch`.
    //
    // ⚠️ THE BRIDGE THE RENDERER CANNOT REBUILD FOR ITSELF. The viewport still
    // draws triangles; the analysis now belongs to surfaces. Something has to say
    // which surface a drawn triangle reads its hours from, and the ONLY place
    // that knows is the flood fill that formed the patches -- by the time a
    // triangle reaches a pixel shader its adjacency, and therefore its patch, is
    // gone.
    //
    // ⚠️ AND IT MUST BE CARRIED FORWARD, NEVER REDISCOVERED. Re-deriving patch
    // identity downstream from positions or normals would agree with this array
    // almost always -- and disagree exactly at the coplanar surfaces that meet
    // at a shared edge, which is the case patch mode exists for. The renderer
    // permutation rule already says it: handed forward, never rediscovered.
    std::vector<uint32_t> patchOfTriangle;

    size_t degenerateFaces = 0;
    // Patches whose lattice held no covered cell at all and that fell back to a
    // single centroid sample. ⚠️ REPORTED, NOT SILENT: coverage is a correctness
    // property and this is the count of surfaces that only just kept it.
    size_t centroidPatches = 0;
    // Surfaces of CONTEXT elements: formed, then given no samples.
    size_t excludedPatches = 0;
    // Distinguishes a refusal from a mesh with only excluded/degenerate patches.
    bool sampleLimitExceeded = false;
    bool valid = false;

    // A triangle that no patch claimed: degenerate, or dropped by the builder.
    // ⚠️ IT IS A REAL CASE AND MUST STAY ADDRESSABLE. A renderer that treated a
    // missing patch as patch 0 would paint one surface's sunlight onto an
    // unrelated triangle, which looks entirely plausible.
    static constexpr uint32_t kNoPatch = 0xffffffffu;

    size_t Count () const
    {
        return areas.size ();
    }
    // ⚠️ SUMMED IN PATCH ORDER RATHER THAN BY std::accumulate OVER A REORDERING.
    // The total is compared against the triangle sampler's, and a different
    // summation order changes the last bits.
    double TotalArea () const
    {
        double total = 0.0;
        for (const double area : areas)
            total += area;
        return total;
    }
};

struct PatchSamplerOptions {
    double spacing = 1.0;
    // How far each sample is lifted along its normal, so a ray does not start on
    // the face it belongs to.
    double normalOffset = 0.01;
    size_t maxSamples = kSamplerMaxSamples;
    SurfacePatchOptions patch;
    // Per group, whether it is an ANALYSIS element -- see
    // SamplerOptions::sampleGroup. A patch never crosses an element, so a
    // patch is wholly analysed or wholly context; a context patch gets no span
    // and its triangles stay `kNoPatch`.
    const std::vector<uint8_t>* sampleGroup = nullptr;
    // Splits coplanar patches at receiver/context boundaries, so an opaque
    // frame cannot become sampled just because it shares a plane with glass.
    const std::vector<uint8_t>* sampleFace = nullptr;
    // The analysis cylinder (SunStudy/SunStudyBounds.hpp), in plan: cells whose
    // centre lies outside it are not sampled. Radius 0 is no clip. Patch domain
    // only -- a triangle tile cannot lose cells without losing its mapping.
    double clipX = 0.0, clipY = 0.0, clipRadius = 0.0;
};

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYPATCHSAMPLER_HPP
