#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

#include <algorithm>
#include <array>

namespace evp::sunstudy::rust {

using namespace detail;

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
