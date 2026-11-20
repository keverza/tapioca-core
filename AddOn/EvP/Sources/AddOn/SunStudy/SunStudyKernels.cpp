#include "SunStudy/SunStudyKernels.hpp"

#include "SunStudy/SunStudyRust.hpp"

namespace evp::sunstudy::kernels {

namespace {

void Report (std::string* error, std::string message)
{
    if (error != nullptr)
        *error = std::move (message);
}

} // namespace

#if !defined(TAPIOCA_GEOMETRY_RUST)
// A build without the Rust core has no sun-study kernels: every call refuses
// with the reason, and the facade returns the kernel's refused answer.
namespace rust {
namespace {
bool NoCore (std::string& error)
{
    error = "this build has no sun-study core";
    return false;
}
} // namespace

bool BuildSunSeries (const std::vector<SunStep>&, int, double, SunSeriesResult&, std::string& error)
{
    return NoCore (error);
}
bool ComputeAnalysisLimits (const MachineResources&, size_t, double, double, AnalysisLimits&, double&,
                            std::string& error)
{
    return NoCore (error);
}
bool EnumerateTimesteps (int, int, int, std::vector<TimeOfDay>&, std::string& error)
{
    return NoCore (error);
}
bool FanSteps (const std::vector<uint16_t>&, double, std::vector<uint32_t>&, std::string& error)
{
    return NoCore (error);
}
bool ScatterToAtlas (const SunStudyAtlas&, const std::vector<double>&, float, std::vector<float>&, std::string& error)
{
    return NoCore (error);
}
bool ResolveElementRoles (const std::vector<std::string>&, const std::vector<std::string>&,
                          const std::vector<std::string>&, const std::vector<std::string>&, ElementRoles&,
                          std::string& error)
{
    return NoCore (error);
}
bool BuildSunStudyReceivers (const geomsrv::Snapshot&, const ElementRoles&, bool, const std::map<int32_t, double>&,
                             double, SunStudyReceivers&, std::string& error)
{
    return NoCore (error);
}
bool SunStudyPresetAdvice (size_t, const std::string&, std::string&, std::string& error)
{
    return NoCore (error);
}
bool BuildSurfaceSampling (const geomsrv::Snapshot&, const std::vector<uint8_t>&, const SurfaceSamplingOptions&,
                           const StudyRecord*, const std::function<bool ()>&, const std::vector<std::vector<uint8_t>>&,
                           SurfaceSamplingResult&, std::string& error)
{
    return NoCore (error);
}
bool FitPatchAtlas (PatchAtlasState&, const PatchSampleGrid&, uint32_t, uint32_t, PatchAtlasUpdate&, std::string& error)
{
    return NoCore (error);
}
bool BuildStableTriangleAtlas (const SampleGrid&, const geomsrv::Snapshot&, PatchAtlasState&, const PatchAtlasState*,
                               const AtlasOptions&, SunStudyAtlas&, std::string& error)
{
    return NoCore (error);
}
bool MakeGroundSampleGrid (const Vec3&, const Vec3&, double, double, double, size_t, GroundGrid&, std::string& error)
{
    return NoCore (error);
}
bool BuildSampleGrid (const double*, size_t, const uint32_t*, size_t, const uint32_t*, const SamplerOptions&,
                      SampleGrid&, std::string& error)
{
    return NoCore (error);
}
bool BuildPatchSampleGrid (const double*, size_t, const uint32_t*, size_t, const uint32_t*,
                           const std::vector<std::string>&, const PatchSamplerOptions&, PatchSampleGrid&,
                           std::string& error)
{
    return NoCore (error);
}
bool BuildSunStudyAtlas (const SampleGrid&, const AtlasOptions&, SunStudyAtlas&, std::string& error)
{
    return NoCore (error);
}
bool RefreshSelectionBinding (SunStudySelectionBinding&, uint64_t, const std::vector<std::string>&,
                              const std::vector<std::string>&, std::vector<std::string>&, std::vector<std::string>&,
                              const std::vector<std::string>&, std::vector<std::string>*, SelectionBindingRefresh&,
                              std::string& error)
{
    return NoCore (error);
}
template <typename T>
bool AtlasChangedRegions (uint32_t, uint32_t, uint32_t, const std::vector<T>&, const std::vector<T>&, size_t,
                          std::vector<AtlasRegion>&, std::string& error)
{
    return NoCore (error);
}
bool OrientOutward (const std::vector<double>&, const std::vector<uint32_t>&, const std::vector<uint32_t>&, double,
                    std::vector<uint32_t>&, WindingReport&, std::string& error)
{
    return NoCore (error);
}
} // namespace rust
#endif

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

AnalysisLimitsReport ReportAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy,
                                           double analysedArea, std::string* error)
{
    AnalysisLimitsReport report;
    std::string message;
    if (!rust::ComputeAnalysisLimits (machine, steps, occupancy, analysedArea, report.limits, report.finestGridSpacing,
                                      message)) {
        Report (error, std::move (message));
        return AnalysisLimitsReport ();
    }
    return report;
}

std::vector<TimeOfDay> EnumerateTimesteps (int timestepMinutes, int hourFrom, int hourTo, std::string* error)
{
    std::vector<TimeOfDay> times;
    std::string message;
    if (!rust::EnumerateTimesteps (timestepMinutes, hourFrom, hourTo, times, message)) {
        Report (error, std::move (message));
        return {};
    }
    return times;
}

std::vector<uint32_t> FanSteps (const std::vector<uint16_t>& stepMinutes, double intervalMinutes, std::string* error)
{
    std::vector<uint32_t> fan;
    std::string message;
    if (!rust::FanSteps (stepMinutes, intervalMinutes, fan, message)) {
        Report (error, std::move (message));
        return {};
    }
    return fan;
}

std::vector<float> ScatterToAtlas (const SunStudyAtlas& atlas, const std::vector<double>& perSample, float emptyValue,
                                   std::string* error)
{
    std::vector<float> image;
    std::string message;
    if (!rust::ScatterToAtlas (atlas, perSample, emptyValue, image, message)) {
        Report (error, std::move (message));
        return {};
    }
    return image;
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

ElementRoles ResolveElementRoles (const std::vector<std::string>& elements,
                                  const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked, std::string* error)
{
    ElementRoles roles;
    std::string message;
    if (!rust::ResolveElementRoles (elements, analysisPicked, contextPicked, ignoredPicked, roles, message)) {
        Report (error, std::move (message));
        return ElementRoles ();
    }
    return roles;
}

std::vector<uint32_t> OrientOutward (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                     size_t faceCount, const uint32_t* groups, size_t groupCount, WindingReport& report,
                                     double weldTolerance, std::string* error)
{
    const std::vector<double> points (vertices, vertices + (vertices != nullptr ? vertexCount * 3 : 0));
    const std::vector<uint32_t> faces (triangles, triangles + (triangles != nullptr ? faceCount * 3 : 0));
    // Fewer ids than faces flips nothing (the C++ contract): send none, and the
    // Rust kernel reads the whole soup as one element -- unless ids were given
    // short, which is refused below rather than guessed.
    std::vector<uint32_t> ids;
    if (groups != nullptr && groupCount >= faceCount)
        ids.assign (groups, groups + faceCount);
    report = WindingReport ();
    if (groups != nullptr && groupCount < faceCount) {
        report.groups = 0;
        return faces;
    }
    std::vector<uint32_t> out;
    std::string message;
    if (!rust::OrientOutward (points, faces, ids, weldTolerance, out, report, message)) {
        Report (error, std::move (message));
        return faces;
    }
    return out;
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

template std::vector<AtlasRegion> AtlasChangedRegions<float> (uint32_t, uint32_t, uint32_t, const std::vector<float>&,
                                                              const std::vector<float>&, size_t,
                                                              const std::function<bool ()>&, std::string*);
template std::vector<AtlasRegion> AtlasChangedRegions<uint32_t> (uint32_t, uint32_t, uint32_t,
                                                                 const std::vector<uint32_t>&,
                                                                 const std::vector<uint32_t>&, size_t,
                                                                 const std::function<bool ()>&, std::string*);

} // namespace evp::sunstudy::kernels
