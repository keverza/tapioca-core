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
    size_t maxSamples = 4000000;
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
};

// Exact canonical-GUID geometry/settings/membership checks precede reuse of
// winding, cells and layouts. No sunlight is copied here; shadow invalidation
// remains ReuseUnaffectedSamples' job. The source must remain immutable/owned.
// Cancellation/refusal returns no partial grid. Checks are between meshes;
// a single mesh's existing sampler is still indivisible.
SurfaceSamplingResult BuildSurfaceSampling (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& sampleMask,
                                            const SurfaceSamplingOptions& options,
                                            const StudyRecord* previous = nullptr,
                                            const std::function<bool ()>& isCancelled = {},
                                            const std::vector<std::vector<uint8_t>>& faceMasks = {});

} // namespace evp::sunstudy
#endif
