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

} // namespace evp::sunstudy::rust
