#ifndef EVP_SUNSTUDY_SUNSTUDYCACHE_HPP
#define EVP_SUNSTUDY_SUNSTUDYCACHE_HPP

// SunStudy/SunStudyCache — the persistent analysis, and which parts of it a
// change invalidates.
//
// ---------------------------------------------------------------------------
// WHY A CACHE REPLACES A STUDY
//
// A study was one indivisible result: it was current or it was thrown away. So
// moving one wall discarded the sunlight on every other surface in the building
// -- ninety per cent of it still exactly correct -- and did so at the moment the
// user was reading it. The overlay vanished, a whole replacement ran, and the
// picture came back some time later.
//
// The cache is the same information addressed by SURFACE instead. A change
// marks the patches it can possibly affect; the rest keep their values, stay on
// screen and cost nothing to "recompute". A complete study is then not an
// object but a condition: every patch is Current.
//
// ⚠️ NO ACAPI, NO Diligent, NO CLOCK, NO RAYCASTER. Which patches a change can
// affect is a geometric argument, and a wrong answer to it does not fail -- it
// leaves a stale shadow on a surface that should have been updated, in an image
// that looks entirely finished. That is exactly the class of claim that has to
// be settled numerically, offline, against a full recomputation.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "SunStudy/SurfacePatch.hpp"

namespace evp::sunstudy {

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

    bool operator< (const PatchKey& other) const
    {
        if (element != other.element)
            return element < other.element;
        return plane < other.plane;
    }
    bool operator== (const PatchKey& other) const
    {
        return element == other.element && plane == other.plane;
    }
};

// ⚠️ THE QUANTUM IS A DECISION, NOT A DETAIL. Too fine and floating-point noise
// between two extractions of the same wall gives it two identities, so the cache
// re-analyses a building that did not change. Too coarse and a wall nudged by
// less than the quantum keeps the identity -- and therefore the sunlight -- of
// where it used to be. A millimetre is far below anything a person models and
// far above extraction noise.
constexpr double kPatchKeyQuantumM = 1.0e-3;

// The key for one patch of one element.
PatchKey MakePatchKey (const std::string& element, const SurfacePatch& patch);

enum class PatchState : uint8_t {
    // Valid for the world as it stands.
    Current,
    // Known invalid, still on screen as the last thing measured here.
    // ⚠️ DRAWN, NOT BLANKED -- see the file header.
    Dirty,
    // Being recomputed; the old values are still what is on screen.
    Computing,
    // The surface itself changed, so the old values describe geometry that is no
    // longer there. ⚠️ THIS ONE MUST NOT BE DRAWN: stretching an old result over
    // a wall that moved is the one case where showing stale data is a lie rather
    // than a delay. The renderer falls through to ordinary shading.
    Unmappable,
};

// One surface's measured sunlight.
struct PatchAnalysis {
    PatchKey key;
    // Where the samples are, in the patch's own lattice. Parallel arrays, one
    // entry per emitted cell.
    std::vector<double> positions; // xyz, lifted off the surface
    std::vector<double> normals;   // xyz unit
    std::vector<double> areas;     // m² carried by each sample, clipped at the boundary
    // Sun hours per sample. Empty until the patch has been resolved once.
    std::vector<double> hours;
    // The patch's world AABB, kept so dirty classification needs no geometry.
    double boundsMin[3] = { 0.0, 0.0, 0.0 };
    double boundsMax[3] = { 0.0, 0.0, 0.0 };
    PatchState state = PatchState::Dirty;

    size_t SampleCount () const
    {
        return areas.size ();
    }
};

// The long-lived analysis. ⚠️ THIS, NOT A STUDY ID, IS THE OBJECT THAT PERSISTS.
struct SunStudyCache {
    // What every patch in here was measured against. A change to any of these
    // invalidates ALL of them -- see `InvalidateAll`.
    uint64_t sunHash = 0;
    uint64_t samplingHash = 0;
    double spacing = 1.0;
    double hoursPerStep = 1.0;

    std::map<PatchKey, PatchAnalysis> patches;

    size_t CountInState (PatchState state) const;
    bool FullyCurrent () const;
    size_t SampleCount () const;
    // Every patch that is not Current, in a deterministic order.
    std::vector<PatchKey> DirtyKeys () const;
};

// An axis-aligned box, and what a change to one looks like.
struct ChangedBounds {
    double min[3] = { 0.0, 0.0, 0.0 };
    double max[3] = { 0.0, 0.0, 0.0 };
    bool valid = false;

    void Add (const double point[3]);
    bool Intersects (const double otherMin[3], const double otherMax[3]) const;
};

// What a geometry change did, as the classifier needs it.
//
// ⚠️ OLD *AND* NEW BOUNDS, AND A MOVE NEEDS BOTH. A wall that moved stopped
// casting a shadow where it was and started casting one where it is; dirtying
// only the new position leaves the old shadow burned into the floor, and every
// total stays plausible.
struct GeometryChange {
    std::string element;
    ChangedBounds oldBounds;
    ChangedBounds newBounds;
};

// Which patches a set of changes can possibly affect.
//
// ⚠️ IT MAY OVER-DIRTY AND IT MUST NEVER UNDER-DIRTY, and the two are not
// comparable mistakes. A false positive costs a patch's worth of rays. A false
// negative leaves a shadow on a surface the sun now reaches -- a finished,
// plausible, wrong picture that nothing downstream can detect.
//
// The argument: a change's shadow can only fall on what lies within the volume
// swept by its bounds along the direction the light travels. Sweeping the box
// for every sun direction in the study and taking the union of those boxes is a
// conservative envelope of that volume -- larger than it, never smaller.
//
// `sunDirections` is xyz-interleaved unit vectors pointing TOWARD the sun, one
// per timestep. `sceneExtent` is how far a shadow may reach -- the scene's
// diagonal is the safe answer.
std::set<PatchKey> ClassifyDirtyPatches (const SunStudyCache& cache, const std::vector<GeometryChange>& changes,
                                         const double* sunDirections, size_t directionCount, double sceneExtent);

// The swept envelope for one change, exposed because it is the whole geometric
// claim and deserves its own test.
ChangedBounds ShadowInfluenceEnvelope (const ChangedBounds& bounds, const double* sunDirections, size_t directionCount,
                                       double sceneExtent);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYCACHE_HPP
