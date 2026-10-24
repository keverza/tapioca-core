#ifndef EVP_SUNSTUDY_SUNSTUDYRUST_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUST_HPP

// SunStudy/SunStudyRust — the C++ face of the Rust sun-study core.
//
// Thin, SDK-free wrappers over SunStudyRustFfi.h: they own the error buffer,
// translate the existing C++ value types to POD views and back, and turn a
// refused call into `false` plus a message. Nothing in production calls them
// yet — the C++ core stays authoritative and is the parity oracle
// (test_sunstudy_rust_parity.cpp) until the TAPIOCA_SUNSTUDY_CORE flip.
//
// The signatures deliberately speak the C++ core's own types so that the flip
// swaps an implementation, not a vocabulary.

#include "SunStudy/SunSeries.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyLimits.hpp"
#include "SunStudy/SunStudyOcclusion.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudyPreset.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudySampler.hpp"
#include "SunStudy/SunStudySelectionBinding.hpp"
#include "SunStudy/SunStudyStepAtlas.hpp"
#include "SunStudy/SunStudyTextureDelta.hpp"
#include "SunStudy/SunStudyWinding.hpp"

#include "SunStudy/SunStudyRustFfi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace evp::sunstudy::rust {

// The linked archive's sun-study ABI version; refuses a header/archive skew.
bool ReadAbiVersion (uint32_t& outVersion, std::string& error);

// SunSeries::FromSteps, with the series' read-outs flattened.
struct SunSeriesResult {
    std::vector<SunStep> steps;
    size_t sourceStepCount = 0;
    uint64_t version = 0;
    int timestepMinutes = 0;
    double hoursPerStep = 0.0;
    double daylightHours = 0.0;
};
bool BuildSunSeries (const std::vector<SunStep>& steps, int timestepMinutes, double minAltitudeDegrees,
                     SunSeriesResult& out, std::string& error);

// FitOrthoFrustum plus the matrices and bias the shadow pass derives from it.
// `frustum.valid == false` (and everything else default) for a degenerate sun
// or box, which is an answer, not an error.
struct ShadowCameraResult {
    OrthoFrustum frustum;
    Mat4 projection {};
    Mat4 lightViewProjection {};
    Vec3 rtcOrigin {};
    Mat4 rtcLightViewProjection {};
    double texelWorldSize = 0.0;
    double bias = 0.0;
    double depthBiasNdc = 0.0;
};
bool FitShadowCamera (const Vec3& aabbMin, const Vec3& aabbMax, const Vec3& sunDir, double margin,
                      uint32_t shadowMapResolution, double minBias, ShadowCameraResult& out, std::string& error);

bool ComputeAccumLayout (size_t sampleCount, uint32_t maxWidth, AccumLayout& out, std::string& error);

bool GridLadder (double areaM2, double targetGrid, double coarseThreshold, double midThreshold,
                 std::vector<double>& out, std::string& error);

// MakeGroundSampleGrid; `out.valid == false` when refused.
bool MakeGroundSampleGrid (const Vec3& aabbMin, const Vec3& aabbMax, double spacing, double pad, double zOffset,
                           size_t maxSamples, GroundGrid& out, std::string& error);

// OrientOutward. Empty `groups` means one element. Refuses (false) where the
// C++ original would read past `groups`.
bool OrientOutward (const std::vector<double>& vertices, const std::vector<uint32_t>& triangles,
                    const std::vector<uint32_t>& groups, double weldTolerance, std::vector<uint32_t>& out,
                    WindingReport& report, std::string& error);

// ComputeAnalysisLimits (the adapter name is not transported) plus the finest
// grid that keeps `analysedArea` within the ceiling.
bool ComputeAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy, double analysedArea,
                            AnalysisLimits& out, double& finestGridSpacing, std::string& error);

// ResolveElementRoles over GUID lists.
bool ResolveElementRoles (const std::vector<std::string>& elements, const std::vector<std::string>& analysisPicked,
                          const std::vector<std::string>& contextPicked, const std::vector<std::string>& ignoredPicked,
                          ElementRoles& out, std::string& error);

// BuildSunStudyReceivers. A role count that disagrees with the mesh count
// returns the roles unchanged and no faces, as the C++ original does, without
// crossing the ABI.
bool BuildSunStudyReceivers (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, bool glassOnly,
                             const std::map<int32_t, double>& transparency, double threshold, SunStudyReceivers& out,
                             std::string& error);

bool SunStudyPresetAdvice (size_t triangleCount, const std::string& selectedPreset, std::string& out,
                           std::string& error);

// SunStudySelectionBinding::Refresh with the decision made in Rust: the same
// list replacement and the same reset of `binding` on Detached.
bool RefreshSelectionBinding (SunStudySelectionBinding& binding, uint64_t currentGeneration,
                              const std::vector<std::string>& context, const std::vector<std::string>& ignored,
                              std::vector<std::string>& studyContext, std::vector<std::string>& studyIgnored,
                              const std::vector<std::string>& analysis, std::vector<std::string>* studyAnalysis,
                              SelectionBindingRefresh& out, std::string& error);

// BuildSampleGrid. `groups` is one id per face or null. A refused grid
// (`valid == false`) carries the counters but no samples.
bool BuildSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                      const uint32_t* groups, const SamplerOptions& options, SampleGrid& out, std::string& error);

// BuildPatchSampleGrid (including BuildSurfacePatches, BuildPatchGrid and the
// patch keys). Same contract as the C++ function.
bool BuildPatchSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                           const uint32_t* groups, const std::vector<std::string>& elementOf,
                           const PatchSamplerOptions& options, PatchSampleGrid& out, std::string& error);

// BuildSunStudyAtlas.
bool BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options, SunStudyAtlas& out, std::string& error);

// A patch atlas held by value: what SunStudyPatchAtlas keeps privately (the
// shelf cursor and generation counter) made explicit, so the state can cross
// the ABI. Default-constructed it equals a fresh SunStudyPatchAtlas.
struct PatchAtlasState {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t penX = 0;
    uint32_t penY = 0;
    uint32_t shelfHeight = 0;
    uint32_t nextGeneration = 1;
    std::map<PatchKey, PatchAtlasAllocation> allocations;
};

// SunStudyPatchAtlas::Fit on `state`.
bool FitPatchAtlas (PatchAtlasState& state, const PatchSampleGrid& grid, uint32_t gutter, uint32_t maxDimension,
                    PatchAtlasUpdate& out, std::string& error);

// BuildStableTriangleAtlas with `allocations` in/out and an optional previous
// study's state.
bool BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot, PatchAtlasState& allocations,
                               const PatchAtlasState* previous, const AtlasOptions& options, SunStudyAtlas& out,
                               std::string& error);

// PackStepMasks over the store's packed step bits (sample-major, LSB-first,
// `stepCount` bits per sample) instead of a callback.
bool PackStepMasks (size_t sampleCount, size_t stepCount, const std::vector<uint8_t>& stepBits,
                    const std::vector<int64_t>& texelOfSample, uint32_t width, uint32_t height, StepMaskAtlas& out,
                    std::string& error);

// AtlasChangedRegions for 32-bit texels (float or uint32_t images).
template <typename T>
bool AtlasChangedRegions (uint32_t width, uint32_t height, uint32_t layers, const std::vector<T>& before,
                          const std::vector<T>& after, size_t maxRegionsPerLayer, std::vector<AtlasRegion>& out,
                          std::string& error);

// StepMinutes, SolarNoonStep and FanSteps for a series.
bool StepSchedule (const SunSeries& series, double fanIntervalMinutes, std::vector<uint16_t>& minutes,
                   std::vector<uint32_t>& fan, uint32_t& solarNoon, std::string& error);

// OcclusionAccumulator backed by the Rust core. Same contract as the C++
// class: one thread at a time, AccumulateStep commits a whole step or nothing.
// A failed traversal or ABI refusal is reported through `LastError()` and the
// call returns false. Movable, not copyable; the Rust handle is released in
// the destructor.
class RustOcclusionAccumulator final {
  public:
    RustOcclusionAccumulator (size_t sampleCount, size_t stepCount);
    ~RustOcclusionAccumulator ();
    RustOcclusionAccumulator (RustOcclusionAccumulator&& other) noexcept;
    RustOcclusionAccumulator& operator= (RustOcclusionAccumulator&& other) noexcept;
    RustOcclusionAccumulator (const RustOcclusionAccumulator&) = delete;
    RustOcclusionAccumulator& operator= (const RustOcclusionAccumulator&) = delete;

    bool Valid () const
    {
        return handle_ != nullptr;
    }
    const std::string& LastError () const
    {
        return error_;
    }

    void Reset ();
    bool SeedReusable (const RustOcclusionAccumulator& source, const std::vector<size_t>& sourceSamples,
                       const std::vector<uint64_t>& dirtySteps = {});
    bool AccumulateStep (const ITraversal& traversal, const SampleSet& samples, size_t stepIndex,
                         const double sunDirection[3], double tmin = 0.001, double tmax = 0.0, size_t maxParallel = 0,
                         const std::function<bool ()>& isCancelled = {});
    size_t AccumulateRange (const ITraversal& traversal, const SampleSet& samples, const SunSeries& series,
                            size_t firstStep, size_t maxSteps, double tmin = 0.001, double tmax = 0.0,
                            size_t maxParallel = 0, const std::function<bool ()>& isCancelled = {});

    size_t SampleCount () const;
    size_t StepCount () const;
    size_t WordsPerSample () const;
    size_t ResolvedStepCount () const;
    size_t ActiveSampleCount () const;
    size_t LastRayCount () const;
    bool Complete () const;
    std::vector<uint64_t> Bits () const;
    std::vector<uint8_t> StepsResolved () const;
    std::vector<double> SunHours (double hoursPerStep) const;

  private:
    TapiocaOcclusionState State () const;
    TapiocaSunStudyOcclusion* handle_ = nullptr;
    mutable std::string error_;
};

} // namespace evp::sunstudy::rust

#endif
