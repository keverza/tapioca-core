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
#include "SunStudy/SunStudyLimits.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyWinding.hpp"

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

} // namespace evp::sunstudy::rust

#endif
