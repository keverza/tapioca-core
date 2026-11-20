#ifndef EVP_SUNSTUDY_SUNSTUDYKERNELS_HPP
#define EVP_SUNSTUDY_SUNSTUDYKERNELS_HPP

// SunStudy/SunStudyKernels -- the one entry production code uses for the
// sun-study kernels (P7b of the Rust migration).
//
// Each function keeps the C++ kernel's signature and answer. A build that links
// the Rust core runs the Rust kernel (pinned bit for bit to the C++ one by
// test_sunstudy_rust_parity.cpp); a build without it runs the C++ body. When the
// C++ bodies are deleted only this file's implementation changes.
//
// A refused Rust call (an ABI failure: a version skew the study handshake
// already refuses, or a caught panic) returns the kernel's own refused answer --
// `valid == false`, an empty list, or default limits -- and puts the message in
// `error` when one is given. It never falls back to the C++ body.

#include "SunStudy/SunSeries.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyLimits.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudyPreset.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudySampler.hpp"
#include "SunStudy/SunStudySelectionBinding.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"
#include "SunStudy/SunStudyTextureDelta.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace evp::sunstudy {
struct StudyRecord;
}

namespace evp::sunstudy::kernels {

// SunSeries::FromSteps.
SunSeries SeriesFromSteps (const std::vector<SunStep>& steps, int timestepMinutes, double minAltitudeDegrees = 0.0,
                           std::string* error = nullptr);

AnalysisLimits ComputeAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy,
                                      std::string* error = nullptr);

// The limits plus the finest grid spacing (metres) that keeps `analysedArea`
// (m2) within the sample ceiling; 0 for no area.
struct AnalysisLimitsReport {
    AnalysisLimits limits;
    double finestGridSpacing = 0.0;
};
AnalysisLimitsReport ReportAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy,
                                           double analysedArea, std::string* error = nullptr);

// EnumerateTimesteps: the clock times of [hourFrom, hourTo) at the timestep.
std::vector<TimeOfDay> EnumerateTimesteps (int timestepMinutes, int hourFrom = 0, int hourTo = 24,
                                           std::string* error = nullptr);

// FanSteps: the steps the HUD's sun fan draws, from the steps' minutes of day.
std::vector<uint32_t> FanSteps (const std::vector<uint16_t>& stepMinutes, double intervalMinutes,
                                std::string* error = nullptr);

// ScatterToAtlas: per-sample values into an atlas image; empty when refused.
std::vector<float> ScatterToAtlas (const SunStudyAtlas& atlas, const std::vector<double>& perSample,
                                   float emptyValue = -1.0f, std::string* error = nullptr);

// ResolveElementRoles over the snapshot's meshes.
ElementRoles ResolveElementRoles (const geomsrv::Snapshot& snapshot, const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked = {}, std::string* error = nullptr);

SunStudyReceivers BuildSunStudyReceivers (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, bool glassOnly,
                                          const std::map<int32_t, double>& transparency, double threshold,
                                          std::string* error = nullptr);

std::string SunStudyPresetAdvice (size_t triangleCount, const std::string& selectedPreset,
                                  std::string* error = nullptr);

// A refused Rust call returns an invalid result, as a refused sampling does.
SurfaceSamplingResult BuildSurfaceSampling (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& sampleMask,
                                            const SurfaceSamplingOptions& options,
                                            const StudyRecord* previous = nullptr,
                                            const std::function<bool ()>& isCancelled = {},
                                            const std::vector<std::vector<uint8_t>>& faceMasks = {},
                                            std::string* error = nullptr);

// atlas.Fit (grid, gutter, maxDimension). A refused Rust call leaves `atlas`
// as it was and returns a default update.
PatchAtlasUpdate FitPatchAtlas (SunStudyPatchAtlas& atlas, const PatchSampleGrid& grid, uint32_t gutter = 1,
                                uint32_t maxDimension = 8192, std::string* error = nullptr);

// A refused Rust call returns an empty atlas and leaves `allocations` as it was.
SunStudyAtlas BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot,
                                        SunStudyPatchAtlas& allocations, const SunStudyPatchAtlas* previous = nullptr,
                                        const AtlasOptions& options = AtlasOptions (), std::string* error = nullptr);

GroundGrid MakeGroundSampleGrid (const Vec3& aabbMin, const Vec3& aabbMax, double spacing, double pad = -1.0,
                                 double zOffset = 0.10, size_t maxSamples = 4000000, std::string* error = nullptr);

SampleGrid BuildSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                            const uint32_t* groups, const SamplerOptions& options, std::string* error = nullptr);

PatchSampleGrid BuildPatchSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                      size_t faceCount, const uint32_t* groups,
                                      const std::vector<std::string>& elementOf,
                                      const PatchSamplerOptions& options = PatchSamplerOptions (),
                                      std::string* error = nullptr);

SunStudyAtlas BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options = AtlasOptions (),
                                  std::string* error = nullptr);

// binding.Refresh (...). A refused Rust call changes nothing and reports
// Unchanged.
SelectionBindingRefresh
RefreshSelectionBinding (SunStudySelectionBinding& binding, uint64_t currentGeneration,
                         const std::vector<std::string>& context, const std::vector<std::string>& ignored,
                         std::vector<std::string>& studyContext, std::vector<std::string>& studyIgnored,
                         const std::vector<std::string>& analysis = {},
                         std::vector<std::string>* studyAnalysis = nullptr, std::string* error = nullptr);

// AtlasChangedRegions for float and uint32_t texels. The Rust diff is not
// cancellable; `isCancelled` is polled before it starts.
template <typename T>
std::vector<AtlasRegion>
AtlasChangedRegions (uint32_t width, uint32_t height, uint32_t layers, const std::vector<T>& before,
                     const std::vector<T>& after, size_t maxRegionsPerLayer = 512,
                     const std::function<bool ()>& isCancelled = {}, std::string* error = nullptr);

} // namespace evp::sunstudy::kernels

#endif
