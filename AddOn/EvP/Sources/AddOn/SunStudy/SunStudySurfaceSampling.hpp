#ifndef EVP_SUNSTUDY_SUNSTUDYSURFACESAMPLING_HPP
#define EVP_SUNSTUDY_SUNSTUDYSURFACESAMPLING_HPP

#include "Geometry/Mesh.hpp"
#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudySampler.hpp"
#include "SunStudy/SunStudyWinding.hpp"

#include <functional>

namespace evp::sunstudy {

struct StudyRecord;

struct SurfaceSamplingOptions {
    SamplingDomain domain = SamplingDomain::TriangleLegacy;
    double spacing = 1.0;
    double normalOffset = 0.01;
    double jitter = 0.0;
    size_t maxSamples = kSamplerMaxSamples;
    // The analysis cylinder (SunStudy/SunStudyBounds.hpp), in plan: cells whose
    // centre lies outside it are not sampled. Radius 0 is no clip. Patch domain
    // only -- a triangle tile cannot lose cells without losing its mapping.
    double clipX = 0.0, clipY = 0.0, clipRadius = 0.0;
};

// Ranges into the record's EXISTING grids, not another copy of the samples.
// Face/group/span indices are rebased when a mesh moves in snapshot order.
struct SurfaceMeshSampling {
    size_t firstFace = 0;
    size_t faceCount = 0;
    size_t firstSample = 0;
    size_t sampleCount = 0;
    size_t firstPatch = 0;
    size_t patchCount = 0;
    size_t degenerate = 0;
    size_t undersized = 0;
    size_t excluded = 0;
    bool sampled = true;
    std::vector<uint8_t> sampleFaces; // empty means all; no retained sample copy
    WindingReport winding;
};

struct SurfaceSamplingLayout {
    SurfaceSamplingOptions options;
    std::vector<SurfaceMeshSampling> meshes;
    bool valid = false;
};

// Why a sampling was refused; mirrors Rust `SamplingRefusal` (C ABI values).
// The host names the real cause instead of blaming the sample ceiling (#117).
enum class SamplingRefusal : uint8_t {
    None = 0,
    InvalidInput = 1,
    SampleLimit = 2,
    NoSurfaces = 3, // patch domain: nothing analysed produced a sample
    Cancelled = 4,
};

struct SurfaceSamplingResult {
    SampleGrid triangles;
    PatchSampleGrid patches;
    SurfaceSamplingLayout layout;
    WindingReport winding;
    size_t reusedMeshes = 0;
    size_t rebuiltMeshes = 0;
    size_t reusedSamples = 0;
    size_t generatedSamples = 0;
    bool valid = false;
    SamplingRefusal refusal = SamplingRefusal::None;
    // Patch domain refused at the sample limit: the least area (m2) the study
    // covers, so the refusal can name the finest grid that fits (#117).
    double areaFloor = 0.0;
};

} // namespace evp::sunstudy
#endif
