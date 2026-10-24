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
static_assert (sizeof (void*) != 8 || sizeof (TapiocaRolesRequest) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaRolesSummary) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaRolesOutput) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaReceiverMeshView) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSunStudyTransparency) == 16);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaReceiversRequest) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaReceiversSummary) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaReceiversOutput) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPresetAdviceRequest) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPresetAdviceOutput) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaBoundList) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaBindingRequest) == 136);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaBindingOutput) == 4);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSampleGridRequest) == 120);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaFaceLayout) == 104);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSampleGridSummary) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSampleGridOutput) == 88);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchGridRequest) == 136);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchSpan) == 192);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchGridSummary) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchGridOutput) == 96);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAtlasSamplesView) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAtlasTile) == 16);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAtlasOutput) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAtlasSummary) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTriangleAtlasRequest) == 72);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchKeyView) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAllocationView) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchShapeView) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAtlasCursor) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAtlasStateView) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAllocation) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAtlasStateOutput) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAtlasUpdate) == 48);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAtlasFitRequest) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchAtlasFitOutput) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAtlasMeshView) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStableAtlasRequest) == 176);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStableAtlasOutput) == 96);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStepMaskRequest) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStepMaskOutput) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTextureDeltaRequest) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTextureDeltaOutput) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStepScheduleRequest) == 24);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaStepScheduleOutput) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaAtlasRegion) == 20);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTraversalVtable) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaOcclusionSeedRequest) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaOcclusionTrace) == 72);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaOcclusionStepRequest) == 104);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaOcclusionRangeRequest) == 104);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaOcclusionState) == 72);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaOcclusionReadOutput) == 56);

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

// Borrowed text views over a string list; valid while `strings` lives.
std::vector<TapiocaTextView> TextViews (const std::vector<std::string>& strings)
{
    std::vector<TapiocaTextView> views;
    views.reserve (strings.size ());
    for (const std::string& s : strings)
        views.push_back (TapiocaTextView { s.data (), s.size () });
    return views;
}

TapiocaTextArrayView ArrayView (const std::vector<TapiocaTextView>& views)
{
    return TapiocaTextArrayView { views.data (), views.size () };
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

bool ResolveElementRoles (const std::vector<std::string>& elements, const std::vector<std::string>& analysisPicked,
                          const std::vector<std::string>& contextPicked, const std::vector<std::string>& ignoredPicked,
                          ElementRoles& out, std::string& error)
{
    const auto e = TextViews (elements);
    const auto a = TextViews (analysisPicked);
    const auto c = TextViews (contextPicked);
    const auto i = TextViews (ignoredPicked);
    const TapiocaRolesRequest request { ArrayView (e), ArrayView (a), ArrayView (c), ArrayView (i) };
    std::vector<uint8_t> roles (elements.size ());
    TapiocaRolesSummary summary {};
    const TapiocaRolesOutput output { roles.data (), roles.size (), &summary };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_resolve_roles (&request, &output, m, n); }, error))
        return false;
    ElementRoles result;
    result.roles.reserve (roles.size ());
    for (const uint8_t role : roles)
        result.roles.push_back (static_cast<ElementRole> (role));
    result.analysis = summary.counts[0];
    result.context = summary.counts[1];
    result.ignored = summary.counts[2];
    result.unmatchedAnalysis = summary.unmatched[0];
    result.unmatchedContext = summary.unmatched[1];
    result.unmatchedIgnored = summary.unmatched[2];
    result.analysisNamedButAbsent = summary.analysisNamedButAbsent != 0;
    out = std::move (result);
    return true;
}

bool BuildSunStudyReceivers (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, bool glassOnly,
                             const std::map<int32_t, double>& transparency, double threshold, SunStudyReceivers& out,
                             std::string& error)
{
    SunStudyReceivers result;
    result.roles = roles;
    if (roles.roles.size () != snapshot.meshes.size ()) {
        out = std::move (result);
        error.clear ();
        return true;
    }
    std::vector<TapiocaReceiverMeshView> meshes;
    meshes.reserve (snapshot.meshes.size ());
    size_t totalFaces = 0;
    for (const geomsrv::Mesh& mesh : snapshot.meshes) {
        meshes.push_back ({ mesh.TriangleCount (), { mesh.triMaterial.data (), mesh.triMaterial.size () } });
        totalFaces += mesh.TriangleCount ();
    }
    std::vector<uint8_t> roleBytes;
    roleBytes.reserve (roles.roles.size ());
    for (const ElementRole role : roles.roles)
        roleBytes.push_back (static_cast<uint8_t> (role));
    std::vector<TapiocaSunStudyTransparency> pool;
    pool.reserve (transparency.size ());
    for (const auto& [material, value] : transparency)
        pool.push_back ({ material, value });
    TapiocaReceiversRequest request {};
    request.meshes = meshes.data ();
    request.meshCount = meshes.size ();
    request.roles = { roleBytes.data (), roleBytes.size () };
    request.transparency = pool.data ();
    request.transparencyCount = pool.size ();
    request.threshold = threshold;
    request.glassOnly = glassOnly ? 1 : 0;
    std::vector<uint8_t> outRoles (meshes.size ());
    std::vector<uint8_t> hasMask (meshes.size ());
    std::vector<uint8_t> faceMask (totalFaces);
    TapiocaReceiversSummary summary {};
    const TapiocaReceiversOutput output { outRoles.data (), hasMask.data (),  meshes.size (),
                                          faceMask.data (), faceMask.size (), &summary };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_receivers (&request, &output, m, n); }, error))
        return false;
    result.faces.resize (meshes.size ());
    size_t offset = 0;
    for (size_t mesh = 0; mesh < meshes.size (); ++mesh) {
        result.roles.roles[mesh] = static_cast<ElementRole> (outRoles[mesh]);
        const size_t count = meshes[mesh].triangleCount;
        if (hasMask[mesh] != 0)
            result.faces[mesh].assign (faceMask.begin () + offset, faceMask.begin () + offset + count);
        offset += count;
    }
    result.roles.analysis = summary.counts[0];
    result.roles.context = summary.counts[1];
    result.roles.ignored = summary.counts[2];
    result.analysisFaces = summary.analysisFaces;
    result.contextFaces = summary.contextFaces;
    result.unknownMaterialFaces = summary.unknownMaterialFaces;
    out = std::move (result);
    return true;
}

bool SunStudyPresetAdvice (size_t triangleCount, const std::string& selectedPreset, std::string& out,
                           std::string& error)
{
    const TapiocaPresetAdviceRequest request { triangleCount, { selectedPreset.data (), selectedPreset.size () } };
    std::array<char, 128> text {};
    size_t length = 0;
    const TapiocaPresetAdviceOutput output { text.data (), text.size (), &length };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_preset_advice (&request, &output, m, n); }, error))
        return false;
    out.assign (text.data (), length);
    return true;
}

bool RefreshSelectionBinding (SunStudySelectionBinding& binding, uint64_t currentGeneration,
                              const std::vector<std::string>& context, const std::vector<std::string>& ignored,
                              std::vector<std::string>& studyContext, std::vector<std::string>& studyIgnored,
                              const std::vector<std::string>& analysis, std::vector<std::string>* studyAnalysis,
                              SelectionBindingRefresh& out, std::string& error)
{
    static const std::vector<std::string> kNone;
    const auto liveC = TextViews (context);
    const auto studyC = TextViews (studyContext);
    const auto liveI = TextViews (ignored);
    const auto studyI = TextViews (studyIgnored);
    const auto liveA = TextViews (analysis);
    const auto studyA = TextViews (studyAnalysis != nullptr ? *studyAnalysis : kNone);
    const auto bound = [] (const std::string& set) { return static_cast<uint8_t> (set.empty () ? 0 : 1); };
    TapiocaBindingRequest request {};
    request.generation = binding.generation;
    request.currentGeneration = currentGeneration;
    request.lists[0] = { 1, bound (binding.contextSet), ArrayView (liveC), ArrayView (studyC) };
    request.lists[1] = { 1, bound (binding.ignoredSet), ArrayView (liveI), ArrayView (studyI) };
    request.lists[2] = { static_cast<uint8_t> (studyAnalysis != nullptr ? 1 : 0), bound (binding.analysisSet),
                         ArrayView (liveA), ArrayView (studyA) };
    TapiocaBindingOutput decision {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_refresh_binding (&request, &decision, m, n); },
                 error))
        return false;
    if (decision.refresh > static_cast<uint8_t> (SelectionBindingRefresh::Detached)) {
        error = "Tapioca Rust sun-study binding returned unknown outcome " + std::to_string (decision.refresh);
        return false;
    }
    out = static_cast<SelectionBindingRefresh> (decision.refresh);
    if (out == SelectionBindingRefresh::Detached)
        binding = {}; // retain the last explicit lists
    if (decision.replace[0] != 0)
        studyContext = context;
    if (decision.replace[1] != 0)
        studyIgnored = ignored;
    if (decision.replace[2] != 0 && studyAnalysis != nullptr)
        *studyAnalysis = analysis;
    return true;
}

bool BuildSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                      const uint32_t* groups, const SamplerOptions& options, SampleGrid& out, std::string& error)
{
    const auto bytes = [] (const std::vector<uint8_t>* mask) {
        return mask == nullptr ? TapiocaByteView { nullptr, 0 } : TapiocaByteView { mask->data (), mask->size () };
    };
    TapiocaSampleGridRequest request {};
    request.vertices = { vertices, vertices == nullptr ? 0 : vertexCount * 3 };
    request.triangles = { triangles, triangles == nullptr ? 0 : faceCount * 3 };
    request.groups = { groups, groups == nullptr ? 0 : faceCount };
    request.sampleGroup = bytes (options.sampleGroup);
    request.sampleFace = bytes (options.sampleFace);
    request.spacing = options.spacing;
    request.normalOffset = options.normalOffset;
    request.jitter = options.jitter;
    request.maxSamples = options.maxSamples;
    request.jitterFaceOffset = options.jitterFaceOffset;
    request.wantLayouts = options.wantLayouts ? 1 : 0;

    TapiocaSampleGridSummary summary {};
    TapiocaSampleGridOutput output {};
    output.summary = &summary;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_sample_grid (&request, &output, m, n); }, error))
        return false;
    SampleGrid grid;
    grid.degenerateFaces = summary.degenerateFaces;
    grid.excludedFaces = summary.excludedFaces;
    grid.undersizedFaces = summary.undersizedFaces;
    grid.valid = summary.valid != 0;
    if (grid.valid && summary.sampleCount != 0) {
        const size_t count = summary.sampleCount;
        grid.positions.resize (count * 3);
        grid.normals.resize (count * 3);
        grid.areas.resize (count);
        grid.faces.resize (count);
        grid.groups.resize (count);
        std::vector<TapiocaFaceLayout> layouts;
        if (options.wantLayouts) {
            grid.cellColumns.resize (count);
            grid.cellRows.resize (count);
            layouts.resize (summary.faceCount);
        }
        output = { grid.positions.data (),
                   grid.normals.data (),
                   grid.areas.data (),
                   grid.faces.data (),
                   grid.groups.data (),
                   options.wantLayouts ? grid.cellColumns.data () : nullptr,
                   options.wantLayouts ? grid.cellRows.data () : nullptr,
                   count,
                   layouts.data (),
                   layouts.size (),
                   &summary };
        if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_sample_grid (&request, &output, m, n); }, error))
            return false;
        for (const TapiocaFaceLayout& pod : layouts) {
            FaceLayout layout;
            std::copy (pod.origin, pod.origin + 3, layout.origin);
            std::copy (pod.uAxis, pod.uAxis + 3, layout.uAxis);
            std::copy (pod.vAxis, pod.vAxis + 3, layout.vAxis);
            layout.uStart = static_cast<long> (pod.uStart);
            layout.vStart = static_cast<long> (pod.vStart);
            layout.columns = pod.columns;
            layout.rows = pod.rows;
            layout.gridded = pod.gridded != 0;
            grid.layouts.push_back (layout);
        }
    }
    else if (grid.valid && options.wantLayouts) {
        // Zero samples means every face was excluded or degenerate (any other
        // face emits at least its centroid), so every layout is the default.
        grid.layouts.assign (summary.faceCount, FaceLayout ());
    }
    out = std::move (grid);
    return true;
}

bool BuildPatchSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles, size_t faceCount,
                           const uint32_t* groups, const std::vector<std::string>& elementOf,
                           const PatchSamplerOptions& options, PatchSampleGrid& out, std::string& error)
{
    const auto bytes = [] (const std::vector<uint8_t>* mask) {
        return mask == nullptr ? TapiocaByteView { nullptr, 0 } : TapiocaByteView { mask->data (), mask->size () };
    };
    const auto names = TextViews (elementOf);
    TapiocaPatchGridRequest request {};
    request.vertices = { vertices, vertices == nullptr ? 0 : vertexCount * 3 };
    request.triangles = { triangles, triangles == nullptr ? 0 : faceCount * 3 };
    request.groups = { groups, groups == nullptr ? 0 : faceCount };
    request.elementOf = ArrayView (names);
    request.sampleGroup = bytes (options.sampleGroup);
    request.sampleFace = bytes (options.sampleFace);
    request.spacing = options.spacing;
    request.normalOffset = options.normalOffset;
    request.coplanarDegrees = options.patch.coplanarDegrees;
    request.planeToleranceM = options.patch.planeToleranceM;
    request.maxSamples = options.maxSamples;

    TapiocaPatchGridSummary summary {};
    TapiocaPatchGridOutput output {};
    output.summary = &summary;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_patch_grid (&request, &output, m, n); }, error))
        return false;
    PatchSampleGrid grid;
    const size_t count = summary.sampleCount;
    grid.positions.resize (count * 3);
    grid.normals.resize (count * 3);
    grid.areas.resize (count);
    grid.cellColumns.resize (count);
    grid.cellRows.resize (count);
    grid.spanOf.resize (count);
    grid.patchOfTriangle.resize (summary.faceCount);
    std::vector<TapiocaPatchSpan> spans (summary.spanCount);
    if (count != 0 || !spans.empty () || summary.faceCount != 0) {
        output = { grid.positions.data (),
                   grid.normals.data (),
                   grid.areas.data (),
                   grid.cellColumns.data (),
                   grid.cellRows.data (),
                   grid.spanOf.data (),
                   count,
                   spans.data (),
                   spans.size (),
                   grid.patchOfTriangle.data (),
                   grid.patchOfTriangle.size (),
                   &summary };
        if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_patch_grid (&request, &output, m, n); }, error))
            return false;
    }
    for (const TapiocaPatchSpan& pod : spans) {
        PatchSampleSpan span;
        span.key.element = pod.group < elementOf.size () ? elementOf[pod.group] : std::string ();
        span.key.plane = pod.plane;
        span.key.occurrence = pod.occurrence;
        span.first = pod.first;
        span.count = pod.count;
        span.columns = pod.columns;
        span.rows = pod.rows;
        std::copy (pod.frame[0], pod.frame[0] + 3, span.origin);
        std::copy (pod.frame[1], pod.frame[1] + 3, span.uAxis);
        std::copy (pod.frame[2], pod.frame[2] + 3, span.vAxis);
        std::copy (pod.frame[3], pod.frame[3] + 3, span.normal);
        std::copy (pod.bounds[0], pod.bounds[0] + 3, span.boundsMin);
        std::copy (pod.bounds[1], pod.bounds[1] + 3, span.boundsMax);
        span.area = pod.area;
        grid.spans.push_back (std::move (span));
    }
    grid.degenerateFaces = summary.degenerateFaces;
    grid.centroidPatches = summary.centroidPatches;
    grid.excludedPatches = summary.excludedPatches;
    grid.sampleLimitExceeded = summary.sampleLimitExceeded != 0;
    grid.valid = summary.valid != 0;
    out = std::move (grid);
    return true;
}

namespace {

// Layout and per-sample views of a grid; valid while `grid` lives.
struct SamplesView {
    std::vector<TapiocaFaceLayout> layouts;
    TapiocaAtlasSamplesView view {};
};

SamplesView ViewOf (const SampleGrid& grid)
{
    SamplesView out;
    out.layouts.reserve (grid.layouts.size ());
    for (const FaceLayout& layout : grid.layouts) {
        TapiocaFaceLayout pod {};
        std::copy (layout.origin, layout.origin + 3, pod.origin);
        std::copy (layout.uAxis, layout.uAxis + 3, pod.uAxis);
        std::copy (layout.vAxis, layout.vAxis + 3, pod.vAxis);
        pod.uStart = layout.uStart;
        pod.vStart = layout.vStart;
        pod.columns = layout.columns;
        pod.rows = layout.rows;
        pod.gridded = layout.gridded ? 1 : 0;
        out.layouts.push_back (pod);
    }
    out.view.layouts = out.layouts.data ();
    out.view.layoutCount = out.layouts.size ();
    out.view.faces = { grid.faces.data (), grid.faces.size () };
    out.view.cellColumns = { grid.cellColumns.data (), grid.cellColumns.size () };
    out.view.cellRows = { grid.cellRows.data (), grid.cellRows.size () };
    return out;
}

// Destination for an atlas; sized for the grid.
struct AtlasBuffers {
    std::vector<TapiocaAtlasTile> tiles;
    std::vector<uint32_t> texels;
    TapiocaAtlasSummary summary {};
    TapiocaAtlasOutput output {};
};

void Prepare (AtlasBuffers& buffers, const SampleGrid& grid)
{
    buffers.tiles.resize (grid.layouts.size ());
    buffers.texels.resize (grid.faces.size ());
    buffers.output = { buffers.tiles.data (), buffers.tiles.size (), buffers.texels.data (), buffers.texels.size (),
                       &buffers.summary };
}

SunStudyAtlas AtlasOf (const AtlasBuffers& buffers)
{
    SunStudyAtlas atlas;
    atlas.width = buffers.summary.width;
    atlas.height = buffers.summary.height;
    for (size_t i = 0; i < buffers.summary.tileCount; ++i) {
        const TapiocaAtlasTile& t = buffers.tiles[i];
        atlas.tiles.push_back ({ t.x, t.y, t.width, t.height });
    }
    atlas.texels.assign (buffers.texels.begin (), buffers.texels.begin () + buffers.summary.texelCount);
    atlas.placedFaces = buffers.summary.placedFaces;
    atlas.valid = buffers.summary.valid != 0;
    return atlas;
}

// A state's allocations as views; valid while `state` lives.
std::vector<TapiocaPatchAllocationView> AllocationViews (const PatchAtlasState& state)
{
    std::vector<TapiocaPatchAllocationView> views;
    views.reserve (state.allocations.size ());
    for (const auto& [key, a] : state.allocations)
        views.push_back ({ { { key.element.data (), key.element.size () }, key.plane, key.occurrence },
                           { a.x, a.y, a.width, a.height, a.generation } });
    return views;
}

TapiocaPatchAtlasStateView StateView (const PatchAtlasState& state,
                                      const std::vector<TapiocaPatchAllocationView>& views)
{
    TapiocaPatchAtlasStateView out {};
    out.cursor = { { state.width, state.height, state.penX, state.penY, state.shelfHeight, state.nextGeneration } };
    out.allocations = views.data ();
    out.allocationCount = views.size ();
    return out;
}

struct StateBuffers {
    TapiocaPatchAtlasCursor cursor {};
    std::vector<TapiocaPatchAllocation> allocations;
    size_t allocationCount = 0;
    std::vector<char> text;
    size_t textLength = 0;
    TapiocaPatchAtlasStateOutput output {};
};

void Prepare (StateBuffers& buffers, size_t allocations, size_t textBytes)
{
    buffers.allocations.resize (allocations);
    buffers.text.resize (textBytes);
    buffers.output = { &buffers.cursor,          buffers.allocations.data (), buffers.allocations.size (),
                       &buffers.allocationCount, buffers.text.data (),        buffers.text.size (),
                       &buffers.textLength };
}

PatchAtlasState StateOf (const StateBuffers& buffers)
{
    PatchAtlasState state;
    const uint32_t* f = buffers.cursor.fields;
    state.width = f[0];
    state.height = f[1];
    state.penX = f[2];
    state.penY = f[3];
    state.shelfHeight = f[4];
    state.nextGeneration = f[5];
    for (size_t i = 0; i < buffers.allocationCount; ++i) {
        const TapiocaPatchAllocation& pod = buffers.allocations[i];
        PatchAtlasAllocation allocation;
        allocation.key.element.assign (buffers.text.data () + pod.elementOffset, pod.elementLength);
        allocation.key.plane = pod.plane;
        allocation.key.occurrence = pod.occurrence;
        allocation.x = pod.rect[0];
        allocation.y = pod.rect[1];
        allocation.width = pod.rect[2];
        allocation.height = pod.rect[3];
        allocation.generation = pod.rect[4];
        state.allocations[allocation.key] = allocation;
    }
    return state;
}

} // namespace

bool BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options, SunStudyAtlas& out, std::string& error)
{
    const SamplesView samples = ViewOf (grid);
    const TapiocaTriangleAtlasRequest request { samples.view, options.gutter, options.maxDimension };
    AtlasBuffers buffers;
    Prepare (buffers, grid);
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_triangle_atlas (&request, &buffers.output, m, n); },
                 error))
        return false;
    out = AtlasOf (buffers);
    return true;
}

bool FitPatchAtlas (PatchAtlasState& state, const PatchSampleGrid& grid, uint32_t gutter, uint32_t maxDimension,
                    PatchAtlasUpdate& out, std::string& error)
{
    const auto views = AllocationViews (state);
    std::vector<TapiocaPatchShapeView> shapes;
    size_t textBytes = 0;
    for (const PatchSampleSpan& span : grid.spans) {
        shapes.push_back (
            { { { span.key.element.data (), span.key.element.size () }, span.key.plane, span.key.occurrence },
              span.columns,
              span.rows });
        textBytes += span.key.element.size ();
    }
    const TapiocaPatchAtlasFitRequest request { StateView (state, views), shapes.data (), shapes.size (), gutter,
                                                maxDimension };
    StateBuffers buffers;
    Prepare (buffers, shapes.size (), textBytes);
    TapiocaPatchAtlasUpdate update {};
    const TapiocaPatchAtlasFitOutput output { buffers.output, &update };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_patch_atlas_fit (&request, &output, m, n); }, error))
        return false;
    state = StateOf (buffers);
    out.retained = update.counts[0];
    out.reused = update.counts[1];
    out.moved = update.counts[2];
    out.added = update.counts[3];
    out.removed = update.counts[4];
    out.resized = update.resized != 0;
    return true;
}

bool BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot, PatchAtlasState& allocations,
                               const PatchAtlasState* previous, const AtlasOptions& options, SunStudyAtlas& out,
                               std::string& error)
{
    const SamplesView samples = ViewOf (grid);
    std::vector<TapiocaAtlasMeshView> meshes;
    size_t triangles = 0;
    size_t textBytes = 0;
    for (const geomsrv::Mesh& mesh : snapshot.meshes) {
        meshes.push_back ({ { mesh.guid.data (), mesh.guid.size () }, mesh.TriangleCount () });
        triangles += mesh.TriangleCount ();
        textBytes += mesh.guid.size ();
    }
    const auto current = AllocationViews (allocations);
    static const PatchAtlasState kEmpty;
    const auto earlier = AllocationViews (previous != nullptr ? *previous : kEmpty);
    TapiocaStableAtlasRequest request {};
    request.samples = samples.view;
    request.meshes = meshes.data ();
    request.meshCount = meshes.size ();
    request.allocations = StateView (allocations, current);
    request.previous = StateView (previous != nullptr ? *previous : kEmpty, earlier);
    request.hasPrevious = previous != nullptr ? 1 : 0;
    request.gutter = options.gutter;
    request.maxDimension = options.maxDimension;
    AtlasBuffers atlas;
    Prepare (atlas, grid);
    StateBuffers state;
    Prepare (state, std::max (triangles, grid.layouts.size ()), textBytes);
    const TapiocaStableAtlasOutput output { atlas.output, state.output };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_stable_triangle_atlas (&request, &output, m, n); },
                 error))
        return false;
    allocations = StateOf (state);
    out = AtlasOf (atlas);
    return true;
}

bool PackStepMasks (size_t sampleCount, size_t stepCount, const std::vector<uint8_t>& stepBits,
                    const std::vector<int64_t>& texelOfSample, uint32_t width, uint32_t height, StepMaskAtlas& out,
                    std::string& error)
{
    const TapiocaStepMaskRequest request { { stepBits.data (), stepBits.size () },
                                           texelOfSample.data (),
                                           texelOfSample.size (),
                                           sampleCount,
                                           stepCount,
                                           width,
                                           height };
    const size_t words = std::max<size_t> (1, (stepCount + 31) / 32);
    StepMaskAtlas atlas;
    atlas.masks.resize (static_cast<size_t> (width) * height * words);
    uint32_t written = 0;
    const TapiocaStepMaskOutput output { atlas.masks.data (), atlas.masks.size (), &written };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_step_masks (&request, &output, m, n); }, error))
        return false;
    atlas.width = width;
    atlas.height = height;
    atlas.words = written;
    atlas.steps = static_cast<uint32_t> (stepCount);
    out = std::move (atlas);
    return true;
}

template <typename T>
bool AtlasChangedRegions (uint32_t width, uint32_t height, uint32_t layers, const std::vector<T>& before,
                          const std::vector<T>& after, size_t maxRegionsPerLayer, std::vector<AtlasRegion>& out,
                          std::string& error)
{
    static_assert (sizeof (T) == sizeof (uint32_t), "32-bit texels only");
    const TapiocaTextureDeltaRequest request { reinterpret_cast<const uint32_t*> (before.data ()),
                                               before.size (),
                                               reinterpret_cast<const uint32_t*> (after.data ()),
                                               after.size (),
                                               { width, height, layers },
                                               maxRegionsPerLayer };
    std::vector<TapiocaAtlasRegion> regions (static_cast<size_t> (layers) * std::max<size_t> (1, maxRegionsPerLayer));
    size_t count = 0;
    const TapiocaTextureDeltaOutput output { regions.data (), regions.size (), &count };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_texture_delta (&request, &output, m, n); }, error))
        return false;
    out.clear ();
    for (size_t i = 0; i < count; ++i)
        out.push_back ({ regions[i].x, regions[i].y, regions[i].width, regions[i].height, regions[i].layer });
    return true;
}

template bool AtlasChangedRegions<float> (uint32_t, uint32_t, uint32_t, const std::vector<float>&,
                                          const std::vector<float>&, size_t, std::vector<AtlasRegion>&, std::string&);
template bool AtlasChangedRegions<uint32_t> (uint32_t, uint32_t, uint32_t, const std::vector<uint32_t>&,
                                             const std::vector<uint32_t>&, size_t, std::vector<AtlasRegion>&,
                                             std::string&);

bool StepSchedule (const SunSeries& series, double fanIntervalMinutes, std::vector<uint16_t>& minutes,
                   std::vector<uint32_t>& fan, uint32_t& solarNoon, std::string& error)
{
    std::vector<TapiocaSunStep> steps;
    for (const SunStep& step : series.Steps ())
        steps.push_back (ToPod (step));
    std::vector<uint16_t> outMinutes (steps.size ());
    std::vector<uint32_t> outFan (steps.size ());
    size_t fanCount = 0;
    uint32_t noon = 0;
    const TapiocaStepScheduleRequest request { { steps.data (), steps.size () }, fanIntervalMinutes };
    const TapiocaStepScheduleOutput output { outMinutes.data (), outFan.data (), steps.size (), &fanCount, &noon };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_step_schedule (&request, &output, m, n); }, error))
        return false;
    outFan.resize (fanCount);
    minutes = std::move (outMinutes);
    fan = std::move (outFan);
    solarNoon = noon;
    return true;
}

namespace {

// The C++ side of a traversal call: the ITraversal and the caller's cancel
// check, reached through the vtable's context pointer for one call.
struct TraversalShim {
    const ITraversal* traversal = nullptr;
    const std::function<bool ()>* isCancelled = nullptr;
};

uint8_t ShimOccludeDirectional (void* context, const double* origins, size_t count, const double* direction,
                                double tmin, double tmax, uint8_t* out, size_t maxParallel)
{
    try {
        const auto* shim = static_cast<const TraversalShim*> (context);
        static const std::function<bool ()> kNever;
        const auto& cancel = shim->isCancelled != nullptr ? *shim->isCancelled : kNever;
        return shim->traversal->OccludeDirectionalCancellable (origins, count, direction, tmin, tmax, out, maxParallel,
                                                               cancel)
                   ? 1
                   : 0;
    }
    catch (...) {
        return 2; // never unwind into Rust
    }
}

uint64_t ShimSceneVersion (void* context)
{
    try {
        return static_cast<const TraversalShim*> (context)->traversal->SceneVersion ();
    }
    catch (...) {
        return 0;
    }
}

uint8_t ShimIsCancelled (void* context)
{
    try {
        const auto* shim = static_cast<const TraversalShim*> (context);
        return shim->isCancelled != nullptr && *shim->isCancelled && (*shim->isCancelled) () ? 1 : 0;
    }
    catch (...) {
        return 1;
    }
}

TapiocaTraversalVtable VtableOf (TraversalShim& shim)
{
    return { &shim, &ShimOccludeDirectional, &ShimSceneVersion, &ShimIsCancelled };
}

TapiocaOcclusionTrace TraceOf (const TapiocaTraversalVtable& vtable, const SampleSet& samples, double tmin, double tmax,
                               size_t maxParallel)
{
    TapiocaOcclusionTrace trace {};
    trace.traversal = &vtable;
    trace.positions = { samples.positions, samples.positions == nullptr ? 0 : samples.count * 3 };
    trace.normals = { samples.normals, samples.normals == nullptr ? 0 : samples.count * 3 };
    trace.sampleCount = samples.count;
    trace.tmin = tmin;
    trace.tmax = tmax;
    trace.maxParallel = maxParallel;
    return trace;
}

} // namespace

RustOcclusionAccumulator::RustOcclusionAccumulator (size_t sampleCount, size_t stepCount)
{
    Invoke (
        [&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_create (sampleCount, stepCount, &handle_, m, n); },
        error_);
}

RustOcclusionAccumulator::~RustOcclusionAccumulator ()
{
    std::string ignored;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_destroy (handle_, m, n); }, ignored);
}

RustOcclusionAccumulator::RustOcclusionAccumulator (RustOcclusionAccumulator&& other) noexcept
    : handle_ (other.handle_), error_ (std::move (other.error_))
{
    other.handle_ = nullptr;
}

RustOcclusionAccumulator& RustOcclusionAccumulator::operator= (RustOcclusionAccumulator&& other) noexcept
{
    if (this != &other) {
        std::string ignored;
        Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_destroy (handle_, m, n); }, ignored);
        handle_ = other.handle_;
        error_ = std::move (other.error_);
        other.handle_ = nullptr;
    }
    return *this;
}

void RustOcclusionAccumulator::Reset ()
{
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_reset (handle_, m, n); }, error_);
}

bool RustOcclusionAccumulator::SeedReusable (const RustOcclusionAccumulator& source,
                                             const std::vector<size_t>& sourceSamples,
                                             const std::vector<uint64_t>& dirtySteps)
{
    const TapiocaOcclusionSeedRequest request { source.handle_, sourceSamples.data (), sourceSamples.size (),
                                                dirtySteps.data (), dirtySteps.size () };
    uint8_t accepted = 0;
    if (!Invoke (
            [&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_seed (handle_, &request, &accepted, m, n); },
            error_))
        return false;
    return accepted != 0;
}

bool RustOcclusionAccumulator::AccumulateStep (const ITraversal& traversal, const SampleSet& samples, size_t stepIndex,
                                               const double sunDirection[3], double tmin, double tmax,
                                               size_t maxParallel, const std::function<bool ()>& isCancelled)
{
    if (sunDirection == nullptr)
        return false;
    TraversalShim shim { &traversal, &isCancelled };
    const TapiocaTraversalVtable vtable = VtableOf (shim);
    TapiocaOcclusionStepRequest request {};
    request.trace = TraceOf (vtable, samples, tmin, tmax, maxParallel);
    request.step = stepIndex;
    std::copy (sunDirection, sunDirection + 3, request.sun);
    uint8_t outcome = 1;
    if (!Invoke (
            [&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_step (handle_, &request, &outcome, m, n); },
            error_))
        return false;
    return outcome == 0;
}

size_t RustOcclusionAccumulator::AccumulateRange (const ITraversal& traversal, const SampleSet& samples,
                                                  const SunSeries& series, size_t firstStep, size_t maxSteps,
                                                  double tmin, double tmax, size_t maxParallel,
                                                  const std::function<bool ()>& isCancelled)
{
    TraversalShim shim { &traversal, &isCancelled };
    const TapiocaTraversalVtable vtable = VtableOf (shim);
    std::vector<double> directions;
    directions.reserve (series.StepCount () * 3);
    for (const SunStep& step : series.Steps ())
        directions.insert (directions.end (), step.direction, step.direction + 3);
    TapiocaOcclusionRangeRequest request {};
    request.trace = TraceOf (vtable, samples, tmin, tmax, maxParallel);
    request.directions = { directions.data (), directions.size () };
    request.first = firstStep;
    request.maxSteps = maxSteps;
    size_t done = 0;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_range (handle_, &request, &done, m, n); },
                 error_))
        return 0;
    return done;
}

TapiocaOcclusionState RustOcclusionAccumulator::State () const
{
    TapiocaOcclusionState state {};
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_state (handle_, &state, m, n); }, error_);
    return state;
}

size_t RustOcclusionAccumulator::SampleCount () const
{
    return State ().sampleCount;
}
size_t RustOcclusionAccumulator::StepCount () const
{
    return State ().stepCount;
}
size_t RustOcclusionAccumulator::WordsPerSample () const
{
    return State ().wordsPerSample;
}
size_t RustOcclusionAccumulator::ResolvedStepCount () const
{
    return State ().resolvedSteps;
}
size_t RustOcclusionAccumulator::ActiveSampleCount () const
{
    return State ().activeSamples;
}
size_t RustOcclusionAccumulator::LastRayCount () const
{
    return State ().lastRayCount;
}
bool RustOcclusionAccumulator::Complete () const
{
    return State ().complete != 0;
}

std::vector<uint64_t> RustOcclusionAccumulator::Bits () const
{
    const TapiocaOcclusionState state = State ();
    std::vector<uint64_t> bits (state.sampleCount * state.wordsPerSample);
    TapiocaOcclusionReadOutput output {};
    output.bits = bits.data ();
    output.bitCapacity = bits.size ();
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_read (handle_, &output, m, n); }, error_);
    return bits;
}

std::vector<uint8_t> RustOcclusionAccumulator::StepsResolved () const
{
    std::vector<uint8_t> flags (State ().stepCount);
    TapiocaOcclusionReadOutput output {};
    output.stepsResolved = flags.data ();
    output.stepCapacity = flags.size ();
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_read (handle_, &output, m, n); }, error_);
    return flags;
}

std::vector<double> RustOcclusionAccumulator::SunHours (double hoursPerStep) const
{
    std::vector<double> hours (State ().sampleCount);
    TapiocaOcclusionReadOutput output {};
    output.sunHours = hours.data ();
    output.hoursCapacity = hours.size ();
    output.hoursPerStep = hoursPerStep;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_occlusion_read (handle_, &output, m, n); }, error_);
    return hours;
}

} // namespace evp::sunstudy::rust
