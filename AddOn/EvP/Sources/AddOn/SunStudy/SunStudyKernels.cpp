#include "SunStudy/SunStudyKernels.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRust.hpp"
#else
#include "SunStudy/SunStudyAtlasReuse.hpp"
#endif

namespace evp::sunstudy::kernels {

namespace {

[[maybe_unused]] void Report (std::string* error, std::string message)
{
    if (error != nullptr)
        *error = std::move (message);
}

} // namespace

#if defined(TAPIOCA_GEOMETRY_RUST)

SunSeries SeriesFromSteps (const std::vector<SunStep>& steps, int timestepMinutes, double minAltitudeDegrees,
                           std::string* error)
{
    rust::SunSeriesResult built;
    std::string message;
    if (!rust::BuildSunSeries (steps, timestepMinutes, minAltitudeDegrees, built, message)) {
        Report (error, std::move (message));
        return SunSeries ();
    }
    return SunSeries::FromParts (std::move (built.steps), built.timestepMinutes, built.sourceStepCount, built.version);
}

AnalysisLimits ComputeAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy,
                                      std::string* error)
{
    AnalysisLimits limits;
    double finest = 0.0;
    std::string message;
    if (!rust::ComputeAnalysisLimits (machine, steps, occupancy, 0.0, limits, finest, message)) {
        Report (error, std::move (message));
        return AnalysisLimits ();
    }
    return limits;
}

ElementRoles ResolveElementRoles (const geomsrv::Snapshot& snapshot, const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked, std::string* error)
{
    std::vector<std::string> guids;
    guids.reserve (snapshot.meshes.size ());
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        guids.push_back (mesh.guid);
    ElementRoles roles;
    std::string message;
    if (!rust::ResolveElementRoles (guids, analysisPicked, contextPicked, ignoredPicked, roles, message)) {
        Report (error, std::move (message));
        return ElementRoles ();
    }
    return roles;
}

SunStudyReceivers BuildSunStudyReceivers (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, bool glassOnly,
                                          const std::map<int32_t, double>& transparency, double threshold,
                                          std::string* error)
{
    SunStudyReceivers receivers;
    std::string message;
    if (!rust::BuildSunStudyReceivers (snapshot, roles, glassOnly, transparency, threshold, receivers, message)) {
        Report (error, std::move (message));
        SunStudyReceivers refused;
        refused.roles = roles;
        return refused;
    }
    return receivers;
}

std::string SunStudyPresetAdvice (size_t triangleCount, const std::string& selectedPreset, std::string* error)
{
    std::string advice;
    std::string message;
    if (!rust::SunStudyPresetAdvice (triangleCount, selectedPreset, advice, message)) {
        Report (error, std::move (message));
        return {};
    }
    return advice;
}

SurfaceSamplingResult BuildSurfaceSampling (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& sampleMask,
                                            const SurfaceSamplingOptions& options, const StudyRecord* previous,
                                            const std::function<bool ()>& isCancelled,
                                            const std::vector<std::vector<uint8_t>>& faceMasks, std::string* error)
{
    SurfaceSamplingResult result;
    std::string message;
    if (!rust::BuildSurfaceSampling (snapshot, sampleMask, options, previous, isCancelled, faceMasks, result,
                                     message)) {
        Report (error, std::move (message));
        return SurfaceSamplingResult ();
    }
    return result;
}

PatchAtlasUpdate FitPatchAtlas (SunStudyPatchAtlas& atlas, const PatchSampleGrid& grid, uint32_t gutter,
                                uint32_t maxDimension, std::string* error)
{
    PatchAtlasState state = atlas.State ();
    PatchAtlasUpdate update;
    std::string message;
    if (!rust::FitPatchAtlas (state, grid, gutter, maxDimension, update, message)) {
        Report (error, std::move (message));
        return PatchAtlasUpdate ();
    }
    atlas = SunStudyPatchAtlas::FromState (std::move (state));
    return update;
}

SunStudyAtlas BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot,
                                        SunStudyPatchAtlas& allocations, const SunStudyPatchAtlas* previous,
                                        const AtlasOptions& options, std::string* error)
{
    PatchAtlasState state = allocations.State ();
    const PatchAtlasState previousState = previous != nullptr ? previous->State () : PatchAtlasState ();
    SunStudyAtlas atlas;
    std::string message;
    if (!rust::BuildStableTriangleAtlas (grid, snapshot, state, previous != nullptr ? &previousState : nullptr, options,
                                         atlas, message)) {
        Report (error, std::move (message));
        return SunStudyAtlas ();
    }
    allocations = SunStudyPatchAtlas::FromState (std::move (state));
    return atlas;
}

GroundGrid MakeGroundSampleGrid (const Vec3& aabbMin, const Vec3& aabbMax, double spacing, double pad, double zOffset,
                                 size_t maxSamples, std::string* error)
{
    GroundGrid grid;
    std::string message;
    if (!rust::MakeGroundSampleGrid (aabbMin, aabbMax, spacing, pad, zOffset, maxSamples, grid, message)) {
        Report (error, std::move (message));
        return GroundGrid ();
    }
    return grid;
}

SampleGrid BuildSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                            const uint32_t* groups, const SamplerOptions& options, std::string* error)
{
    SampleGrid grid;
    std::string message;
    if (!rust::BuildSampleGrid (vertices, vertexCount, triangles, faceCount, groups, options, grid, message)) {
        Report (error, std::move (message));
        return SampleGrid ();
    }
    return grid;
}

PatchSampleGrid BuildPatchSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                      size_t faceCount, const uint32_t* groups,
                                      const std::vector<std::string>& elementOf, const PatchSamplerOptions& options,
                                      std::string* error)
{
    PatchSampleGrid grid;
    std::string message;
    if (!rust::BuildPatchSampleGrid (vertices, vertexCount, triangles, faceCount, groups, elementOf, options, grid,
                                     message)) {
        Report (error, std::move (message));
        return PatchSampleGrid ();
    }
    return grid;
}

SunStudyAtlas BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options, std::string* error)
{
    SunStudyAtlas atlas;
    std::string message;
    if (!rust::BuildSunStudyAtlas (grid, options, atlas, message)) {
        Report (error, std::move (message));
        return SunStudyAtlas ();
    }
    return atlas;
}

SelectionBindingRefresh RefreshSelectionBinding (SunStudySelectionBinding& binding, uint64_t currentGeneration,
                                                 const std::vector<std::string>& context,
                                                 const std::vector<std::string>& ignored,
                                                 std::vector<std::string>& studyContext,
                                                 std::vector<std::string>& studyIgnored,
                                                 const std::vector<std::string>& analysis,
                                                 std::vector<std::string>* studyAnalysis, std::string* error)
{
    SelectionBindingRefresh refreshed = SelectionBindingRefresh::Unchanged;
    std::string message;
    if (!rust::RefreshSelectionBinding (binding, currentGeneration, context, ignored, studyContext, studyIgnored,
                                        analysis, studyAnalysis, refreshed, message)) {
        Report (error, std::move (message));
        return SelectionBindingRefresh::Unchanged;
    }
    return refreshed;
}

template <typename T>
std::vector<AtlasRegion> AtlasChangedRegions (uint32_t width, uint32_t height, uint32_t layers,
                                              const std::vector<T>& before, const std::vector<T>& after,
                                              size_t maxRegionsPerLayer, const std::function<bool ()>& isCancelled,
                                              std::string* error)
{
    if (isCancelled && isCancelled ())
        return {};
    std::vector<AtlasRegion> regions;
    std::string message;
    if (!rust::AtlasChangedRegions (width, height, layers, before, after, maxRegionsPerLayer, regions, message)) {
        Report (error, std::move (message));
        return {};
    }
    return regions;
}

#else // the C++ kernels

SunSeries SeriesFromSteps (const std::vector<SunStep>& steps, int timestepMinutes, double minAltitudeDegrees,
                           std::string*)
{
    return SunSeries::FromSteps (steps, timestepMinutes, minAltitudeDegrees);
}

AnalysisLimits ComputeAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy, std::string*)
{
    return evp::sunstudy::ComputeAnalysisLimits (machine, steps, occupancy);
}

ElementRoles ResolveElementRoles (const geomsrv::Snapshot& snapshot, const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked, std::string*)
{
    return evp::sunstudy::ResolveElementRoles (snapshot, analysisPicked, contextPicked, ignoredPicked);
}

SunStudyReceivers BuildSunStudyReceivers (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, bool glassOnly,
                                          const std::map<int32_t, double>& transparency, double threshold, std::string*)
{
    return evp::sunstudy::BuildSunStudyReceivers (snapshot, roles, glassOnly, transparency, threshold);
}

std::string SunStudyPresetAdvice (size_t triangleCount, const std::string& selectedPreset, std::string*)
{
    return evp::sunstudy::SunStudyPresetAdvice (triangleCount, selectedPreset);
}

SurfaceSamplingResult BuildSurfaceSampling (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& sampleMask,
                                            const SurfaceSamplingOptions& options, const StudyRecord* previous,
                                            const std::function<bool ()>& isCancelled,
                                            const std::vector<std::vector<uint8_t>>& faceMasks, std::string*)
{
    return evp::sunstudy::BuildSurfaceSampling (snapshot, sampleMask, options, previous, isCancelled, faceMasks);
}

PatchAtlasUpdate FitPatchAtlas (SunStudyPatchAtlas& atlas, const PatchSampleGrid& grid, uint32_t gutter,
                                uint32_t maxDimension, std::string*)
{
    return atlas.Fit (grid, gutter, maxDimension);
}

SunStudyAtlas BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot,
                                        SunStudyPatchAtlas& allocations, const SunStudyPatchAtlas* previous,
                                        const AtlasOptions& options, std::string*)
{
    return evp::sunstudy::BuildStableTriangleAtlas (grid, snapshot, allocations, previous, options);
}

GroundGrid MakeGroundSampleGrid (const Vec3& aabbMin, const Vec3& aabbMax, double spacing, double pad, double zOffset,
                                 size_t maxSamples, std::string*)
{
    return evp::sunstudy::MakeGroundSampleGrid (aabbMin, aabbMax, spacing, pad, zOffset, maxSamples);
}

SampleGrid BuildSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                            const uint32_t* groups, const SamplerOptions& options, std::string*)
{
    return evp::sunstudy::BuildSampleGrid (vertices, vertexCount, triangles, faceCount, groups, options);
}

PatchSampleGrid BuildPatchSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                      size_t faceCount, const uint32_t* groups,
                                      const std::vector<std::string>& elementOf, const PatchSamplerOptions& options,
                                      std::string*)
{
    return evp::sunstudy::BuildPatchSampleGrid (vertices, vertexCount, triangles, faceCount, groups, elementOf,
                                                options);
}

SunStudyAtlas BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options, std::string*)
{
    return evp::sunstudy::BuildSunStudyAtlas (grid, options);
}

SelectionBindingRefresh RefreshSelectionBinding (SunStudySelectionBinding& binding, uint64_t currentGeneration,
                                                 const std::vector<std::string>& context,
                                                 const std::vector<std::string>& ignored,
                                                 std::vector<std::string>& studyContext,
                                                 std::vector<std::string>& studyIgnored,
                                                 const std::vector<std::string>& analysis,
                                                 std::vector<std::string>* studyAnalysis, std::string*)
{
    return binding.Refresh (currentGeneration, context, ignored, studyContext, studyIgnored, analysis, studyAnalysis);
}

template <typename T>
std::vector<AtlasRegion> AtlasChangedRegions (uint32_t width, uint32_t height, uint32_t layers,
                                              const std::vector<T>& before, const std::vector<T>& after,
                                              size_t maxRegionsPerLayer, const std::function<bool ()>& isCancelled,
                                              std::string*)
{
    return evp::sunstudy::AtlasChangedRegions (width, height, layers, before, after, maxRegionsPerLayer, isCancelled);
}

#endif

template std::vector<AtlasRegion> AtlasChangedRegions<float> (uint32_t, uint32_t, uint32_t, const std::vector<float>&,
                                                              const std::vector<float>&, size_t,
                                                              const std::function<bool ()>&, std::string*);
template std::vector<AtlasRegion> AtlasChangedRegions<uint32_t> (uint32_t, uint32_t, uint32_t,
                                                                 const std::vector<uint32_t>&,
                                                                 const std::vector<uint32_t>&, size_t,
                                                                 const std::function<bool ()>&, std::string*);

} // namespace evp::sunstudy::kernels
