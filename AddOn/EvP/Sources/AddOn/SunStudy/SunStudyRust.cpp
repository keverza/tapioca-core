#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustFfi.h"

#include <algorithm>
#include <array>

namespace evp::sunstudy::rust {

// Mirrors the Rust-side assertions in ffi/src/sunstudy.rs.
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSunStep) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSunSeriesRequest) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSunSeriesSummary) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSunSeriesOutput) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaShadowCameraRequest) == 96);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaShadowCamera) == 584);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAccumLayout) == 12);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaGridLadderRequest) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaGridLadder) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaGroundGridRequest) == 80);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaGroundGridSummary) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaGroundGridOutput) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaWindingRequest) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaWindingReport) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaWindingOutput) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaLimitsRequest) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaLimits) == 80);

namespace {

constexpr size_t kErrorCapacity = 256;
using ErrorBuffer = std::array<char, kErrorCapacity>;

std::string FailureMessage (TapiocaStatus status, const ErrorBuffer& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    if (length != 0)
        return std::string (buffer.data (), length);
    // A failed status with no diagnostic is still a failure (overlap refusals
    // deliberately write nothing); never turn it into success.
    return "Tapioca Rust sun-study call failed with status " + std::to_string (status);
}

// Runs one export with a fresh error buffer; true on TAPIOCA_OK.
template <typename Call> bool Invoke (Call&& call, std::string& error)
{
    ErrorBuffer buffer {};
    const TapiocaStatus status = call (buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    error.clear ();
    return true;
}

void Copy3 (const Vec3& from, double (&to)[3])
{
    std::copy (from.begin (), from.end (), to);
}

Vec3 ToVec3 (const double (&from)[3])
{
    return { from[0], from[1], from[2] };
}

Mat4 ToMat4 (const double (&from)[16])
{
    Mat4 m {};
    std::copy (from, from + 16, m.begin ());
    return m;
}

TapiocaSunStep ToPod (const SunStep& step)
{
    TapiocaSunStep pod {};
    pod.hour = step.time.hour;
    pod.minute = step.time.minute;
    std::copy (step.direction, step.direction + 3, pod.direction);
    pod.altitudeDegrees = step.altitudeDegrees;
    return pod;
}

SunStep FromPod (const TapiocaSunStep& pod)
{
    SunStep step;
    step.time = TimeOfDay { pod.hour, pod.minute };
    std::copy (pod.direction, pod.direction + 3, step.direction);
    step.altitudeDegrees = pod.altitudeDegrees;
    return step;
}

} // namespace

bool ReadAbiVersion (uint32_t& outVersion, std::string& error)
{
    uint32_t version = 0;
    if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_version (&version, e, c); }, error))
        return false;
    if (version != TAPIOCA_SUNSTUDY_ABI_VERSION) {
        error = "Tapioca Rust sun-study ABI version " + std::to_string (version) + " does not match expected " +
                std::to_string (TAPIOCA_SUNSTUDY_ABI_VERSION);
        return false;
    }
    outVersion = version;
    return true;
}

bool BuildSunSeries (const std::vector<SunStep>& steps, int timestepMinutes, double minAltitudeDegrees,
                     SunSeriesResult& out, std::string& error)
{
    std::vector<TapiocaSunStep> input (steps.size ());
    std::transform (steps.begin (), steps.end (), input.begin (), ToPod);
    std::vector<TapiocaSunStep> kept (steps.size ());
    TapiocaSunSeriesSummary summary {};
    const TapiocaSunSeriesRequest request { { input.data (), input.size () }, timestepMinutes, minAltitudeDegrees };
    const TapiocaSunSeriesOutput output { kept.data (), kept.size (), &summary };
    if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_series (&request, &output, e, c); }, error))
        return false;
    SunSeriesResult result;
    result.steps.reserve (summary.stepCount);
    for (size_t i = 0; i < summary.stepCount; ++i)
        result.steps.push_back (FromPod (kept[i]));
    result.sourceStepCount = summary.sourceStepCount;
    result.version = summary.version;
    result.timestepMinutes = summary.timestepMinutes;
    result.hoursPerStep = summary.hoursPerStep;
    result.daylightHours = summary.daylightHours;
    out = std::move (result);
    return true;
}

bool FitShadowCamera (const Vec3& aabbMin, const Vec3& aabbMax, const Vec3& sunDir, double margin,
                      uint32_t shadowMapResolution, double minBias, ShadowCameraResult& out, std::string& error)
{
    TapiocaShadowCameraRequest request {};
    Copy3 (aabbMin, request.aabbMin);
    Copy3 (aabbMax, request.aabbMax);
    Copy3 (sunDir, request.sunDir);
    request.margin = margin;
    request.resolution = shadowMapResolution;
    request.minBias = minBias;
    TapiocaShadowCamera camera {};
    if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_shadow_camera (&request, &camera, e, c); }, error))
        return false;
    ShadowCameraResult result;
    if (camera.valid != 0) {
        OrthoFrustum& f = result.frustum;
        f.left = camera.planes[0];
        f.right = camera.planes[1];
        f.bottom = camera.planes[2];
        f.top = camera.planes[3];
        f.nearPlane = camera.planes[4];
        f.farPlane = camera.planes[5];
        f.worldWidth = camera.extent[0];
        f.worldHeight = camera.extent[1];
        f.worldDepth = camera.extent[2];
        f.rightAxis = ToVec3 (camera.rightAxis);
        f.upAxis = ToVec3 (camera.upAxis);
        f.lightAxis = ToVec3 (camera.lightAxis);
        f.valid = true;
        result.projection = ToMat4 (camera.projection);
        result.lightViewProjection = ToMat4 (camera.lightViewProjection);
        result.rtcOrigin = ToVec3 (camera.rtcOrigin);
        result.rtcLightViewProjection = ToMat4 (camera.rtcLightViewProjection);
        result.texelWorldSize = camera.texelWorldSize;
        result.bias = camera.bias;
        result.depthBiasNdc = camera.depthBiasNdc;
    }
    out = result;
    return true;
}

bool ComputeAccumLayout (size_t sampleCount, uint32_t maxWidth, AccumLayout& out, std::string& error)
{
    TapiocaAccumLayout layout {};
    if (!Invoke (
            [&] (char* e, size_t c) { return tapioca_sunstudy_accum_layout (sampleCount, maxWidth, &layout, e, c); },
            error))
        return false;
    out.width = layout.width;
    out.height = layout.height;
    out.valid = layout.valid != 0;
    return true;
}

bool GridLadder (double areaM2, double targetGrid, double coarseThreshold, double midThreshold,
                 std::vector<double>& out, std::string& error)
{
    const TapiocaGridLadderRequest request { areaM2, targetGrid, coarseThreshold, midThreshold };
    TapiocaGridLadder ladder {};
    if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_grid_ladder (&request, &ladder, e, c); }, error))
        return false;
    out.assign (ladder.grids, ladder.grids + std::min<uint32_t> (ladder.count, 3));
    return true;
}

bool MakeGroundSampleGrid (const Vec3& aabbMin, const Vec3& aabbMax, double spacing, double pad, double zOffset,
                           size_t maxSamples, GroundGrid& out, std::string& error)
{
    TapiocaGroundGridRequest request {};
    Copy3 (aabbMin, request.aabbMin);
    Copy3 (aabbMax, request.aabbMax);
    request.spacing = spacing;
    request.pad = pad;
    request.zOffset = zOffset;
    request.maxSamples = maxSamples;
    TapiocaGroundGridSummary summary {};
    const TapiocaGroundGridOutput shape { nullptr, 0, &summary };
    if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_ground_grid (&request, &shape, e, c); }, error))
        return false;
    GroundGrid grid;
    if (summary.valid != 0) {
        grid.positions.resize (summary.sampleCount * 3);
        const TapiocaGroundGridOutput fill { grid.positions.data (), grid.positions.size (), &summary };
        if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_ground_grid (&request, &fill, e, c); }, error))
            return false;
        grid.normals.reserve (grid.positions.size ());
        for (size_t i = 0; i < summary.sampleCount; ++i)
            grid.normals.insert (grid.normals.end (), { 0.0, 0.0, 1.0 });
        grid.pad = summary.pad;
        grid.groundZ = summary.groundZ;
        grid.columns = summary.columns;
        grid.rows = summary.rows;
        grid.valid = true;
    }
    out = std::move (grid);
    return true;
}

bool OrientOutward (const std::vector<double>& vertices, const std::vector<uint32_t>& triangles,
                    const std::vector<uint32_t>& groups, double weldTolerance, std::vector<uint32_t>& out,
                    WindingReport& report, std::string& error)
{
    const TapiocaWindingRequest request { { vertices.data (), vertices.size () },
                                          { triangles.data (), triangles.size () },
                                          { groups.data (), groups.size () },
                                          weldTolerance };
    std::vector<uint32_t> oriented (triangles.size () / 3 * 3);
    TapiocaWindingReport pod {};
    const TapiocaWindingOutput output { oriented.data (), oriented.size (), &pod };
    if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_orient_outward (&request, &output, e, c); }, error))
        return false;
    oriented.resize (pod.indexCount);
    report.groups = pod.groups;
    report.closed = pod.closed;
    report.flipped = pod.flipped;
    report.flippedTriangles = pod.flippedTriangles;
    out = std::move (oriented);
    return true;
}

bool ComputeAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy, double analysedArea,
                            AnalysisLimits& out, double& finestGridSpacing, std::string& error)
{
    TapiocaLimitsRequest request {};
    request.gpuMemoryBytes = machine.gpuMemoryBytes;
    request.availableRamBytes = machine.availableRamBytes;
    request.steps = steps;
    request.occupancy = occupancy;
    request.analysedArea = analysedArea;
    request.maxTextureDimension = machine.maxTextureDimension;
    request.gpuKnown = machine.gpuKnown ? 1u : 0u;
    TapiocaLimits limits {};
    if (!Invoke ([&] (char* e, size_t c) { return tapioca_sunstudy_limits (&request, &limits, e, c); }, error))
        return false;
    if (limits.binding > static_cast<uint8_t> (LimitBinding::TextureSize)) {
        error = "Tapioca Rust sun-study limits returned unknown binding " + std::to_string (limits.binding);
        return false;
    }
    AnalysisLimits result;
    result.maxSamples = limits.maxSamples;
    result.maxAtlasDimension = limits.maxAtlasDimension;
    result.binding = static_cast<LimitBinding> (limits.binding);
    result.maxRays = limits.maxRays;
    result.samplerCap = limits.caps[0];
    result.ramCap = limits.caps[1];
    result.gpuCap = limits.caps[2];
    result.textureCap = limits.caps[3];
    out = result;
    finestGridSpacing = limits.finestGridSpacing;
    return true;
}

} // namespace evp::sunstudy::rust
