#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <memory>

namespace evp::sunstudy::rust {

using namespace detail;

static_assert (sizeof (void*) != 8 || sizeof (TapiocaSurfaceOptions) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaMeshSpan) == 136);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSampleGridView) == 160);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchSpanEntry) == 208);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPatchGridView) == 160);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaPreviousSampling) == 400);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSurfaceBuildRequest) == 512);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSurfaceResultSummary) == 224);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSurfaceReadOutput) == 160);

namespace {

TapiocaSurfaceOptions OptionsPod (const SurfaceSamplingOptions& options)
{
    return { options.spacing, options.normalOffset, options.jitter, options.maxSamples,
             static_cast<uint8_t> (options.domain) };
}

PatchSampleSpan SpanOf (const TapiocaPatchSpan& pod, std::string element)
{
    PatchSampleSpan span;
    span.key.element = std::move (element);
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
    return span;
}

TapiocaMeshSpan MeshSpanPod (const SurfaceMeshSampling& m)
{
    TapiocaMeshSpan span {};
    const size_t ranges[9] = { m.firstFace,  m.faceCount,  m.firstSample, m.sampleCount, m.firstPatch,
                               m.patchCount, m.degenerate, m.undersized,  m.excluded };
    std::copy (ranges, ranges + 9, span.ranges);
    const size_t winding[4] = { m.winding.groups, m.winding.closed, m.winding.flipped, m.winding.flippedTriangles };
    std::copy (winding, winding + 4, span.winding);
    span.sampleFaces = { m.sampleFaces.data (), m.sampleFaces.size () };
    span.sampled = m.sampled ? 1 : 0;
    return span;
}

// Views over an earlier record; valid while the record lives.
struct PreviousViews {
    std::vector<TapiocaReuseMeshView> meshes;
    std::vector<TapiocaMeshSpan> spans;
    std::vector<TapiocaFaceLayout> layouts;
    std::vector<TapiocaPatchSpanEntry> patchSpans;
    TapiocaPreviousSampling view {};
};

void Fill (PreviousViews& views, const StudyRecord& record)
{
    views.meshes = MeshViews (*record.snapshot);
    for (const SurfaceMeshSampling& mesh : record.samplingLayout.meshes)
        views.spans.push_back (MeshSpanPod (mesh));
    const SampleGrid& t = record.sampleGrid;
    for (const FaceLayout& layout : t.layouts)
        views.layouts.push_back (LayoutPod (layout));
    const PatchSampleGrid& p = record.patchGrid;
    for (const PatchSampleSpan& span : p.spans)
        views.patchSpans.push_back ({ SpanPod (span), { span.key.element.data (), span.key.element.size () } });
    TapiocaPreviousSampling& v = views.view;
    v.meshes = { views.meshes.data (), views.meshes.size () };
    v.options = OptionsPod (record.samplingLayout.options);
    v.spans = { views.spans.data (), views.spans.size () };
    v.sampleGrid = { { t.positions.data (), t.positions.size () },
                     { t.normals.data (), t.normals.size () },
                     { t.areas.data (), t.areas.size () },
                     { t.faces.data (), t.faces.size () },
                     { t.groups.data (), t.groups.size () },
                     { views.layouts.data (), views.layouts.size () },
                     { t.cellColumns.data (), t.cellColumns.size () },
                     { t.cellRows.data (), t.cellRows.size () },
                     { t.degenerateFaces, t.excludedFaces, t.undersizedFaces },
                     static_cast<uint8_t> (t.valid ? 1 : 0) };
    v.patchGrid = { { p.positions.data (), p.positions.size () },
                    { p.normals.data (), p.normals.size () },
                    { p.areas.data (), p.areas.size () },
                    { p.cellColumns.data (), p.cellColumns.size () },
                    { p.cellRows.data (), p.cellRows.size () },
                    { p.spanOf.data (), p.spanOf.size () },
                    { views.patchSpans.data (), views.patchSpans.size () },
                    { p.patchOfTriangle.data (), p.patchOfTriangle.size () },
                    { p.degenerateFaces, p.centroidPatches, p.excludedPatches },
                    static_cast<uint8_t> (p.sampleLimitExceeded ? 1 : 0),
                    static_cast<uint8_t> (p.valid ? 1 : 0) };
    v.layoutValid = record.samplingLayout.valid ? 1 : 0;
    v.domain = static_cast<uint8_t> (record.domain);
}

uint8_t PollCancelled (void* context)
{
    try {
        const auto& isCancelled = *static_cast<const std::function<bool ()>*> (context);
        return isCancelled && isCancelled () ? 1 : 0;
    }
    catch (...) {
        return 1;
    }
}

struct ResultDeleter {
    void operator() (TapiocaSurfaceResult* handle) const noexcept
    {
        ErrorBuffer buffer {};
        tapioca_sunstudy_surface_destroy (handle, buffer.data (), buffer.size ());
    }
};

using ResultHandle = std::unique_ptr<TapiocaSurfaceResult, ResultDeleter>;

SurfaceMeshSampling MeshSpanOf (const TapiocaMeshSpan& pod, const std::vector<uint8_t>& masks)
{
    SurfaceMeshSampling m;
    m.firstFace = pod.ranges[0];
    m.faceCount = pod.ranges[1];
    m.firstSample = pod.ranges[2];
    m.sampleCount = pod.ranges[3];
    m.firstPatch = pod.ranges[4];
    m.patchCount = pod.ranges[5];
    m.degenerate = pod.ranges[6];
    m.undersized = pod.ranges[7];
    m.excluded = pod.ranges[8];
    m.sampled = pod.sampled != 0;
    const auto first = masks.begin () + static_cast<std::ptrdiff_t> (pod.maskOffset);
    m.sampleFaces.assign (first, first + static_cast<std::ptrdiff_t> (pod.sampleFaces.count));
    m.winding = { pod.winding[0], pod.winding[1], pod.winding[2], pod.winding[3] };
    return m;
}

// Destination buffers for one read, sized from the summary.
struct ReadBuffers {
    std::vector<TapiocaFaceLayout> layouts;
    std::vector<TapiocaPatchSpan> spans;
    std::vector<size_t> offsets;
    std::vector<TapiocaMeshSpan> meshSpans;
    std::vector<uint8_t> masks;
    std::string text;
};

TapiocaSurfaceReadOutput Size (SurfaceSamplingResult& result, ReadBuffers& b, const size_t (&sizes)[8])
{
    SampleGrid& t = result.triangles;
    PatchSampleGrid& p = result.patches;
    for (std::vector<double>* reals : { &t.positions, &t.normals })
        reals->resize (sizes[0] * 3);
    t.areas.resize (sizes[0]);
    for (std::vector<uint32_t>* indices : { &t.faces, &t.groups, &t.cellColumns, &t.cellRows })
        indices->resize (sizes[0]);
    b.layouts.resize (sizes[1]);
    for (std::vector<double>* reals : { &p.positions, &p.normals })
        reals->resize (sizes[2] * 3);
    p.areas.resize (sizes[2]);
    for (std::vector<uint32_t>* indices : { &p.cellColumns, &p.cellRows, &p.spanOf })
        indices->resize (sizes[2]);
    b.spans.resize (sizes[3]);
    b.offsets.resize (sizes[3]);
    p.patchOfTriangle.resize (sizes[4]);
    b.meshSpans.resize (sizes[5]);
    b.masks.resize (sizes[6]);
    b.text.assign (sizes[7], '\0');
    return { { t.positions.data (), t.normals.data (), t.areas.data () },
             { t.faces.data (), t.groups.data (), t.cellColumns.data (), t.cellRows.data () },
             b.layouts.data (),
             { p.positions.data (), p.normals.data (), p.areas.data () },
             { p.cellColumns.data (), p.cellRows.data (), p.spanOf.data (), p.patchOfTriangle.data () },
             b.spans.data (),
             b.offsets.data (),
             b.text.data (),
             b.meshSpans.data (),
             b.masks.data () };
}

// Copies a result out of its handle.
} // namespace

namespace detail {

bool Read (const TapiocaSurfaceResult* handle, SurfaceSamplingResult& out, std::string& error)
{
    TapiocaSurfaceResultSummary summary {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_surface_summary (handle, &summary, m, n); }, error))
        return false;
    SurfaceSamplingResult result;
    ReadBuffers b;
    const TapiocaSurfaceReadOutput output = Size (result, b, summary.sizes);
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_surface_read (handle, &output, m, n); }, error))
        return false;
    SampleGrid& t = result.triangles;
    PatchSampleGrid& p = result.patches;
    for (const TapiocaFaceLayout& pod : b.layouts)
        t.layouts.push_back (LayoutOf (pod));
    for (size_t i = 0; i < b.spans.size (); ++i) {
        const size_t end = i + 1 < b.offsets.size () ? b.offsets[i + 1] : b.text.size ();
        p.spans.push_back (SpanOf (b.spans[i], b.text.substr (b.offsets[i], end - b.offsets[i])));
    }
    t.degenerateFaces = summary.triangleCounters[0];
    t.excludedFaces = summary.triangleCounters[1];
    t.undersizedFaces = summary.triangleCounters[2];
    t.valid = summary.flags[1] != 0;
    p.degenerateFaces = summary.patchCounters[0];
    p.centroidPatches = summary.patchCounters[1];
    p.excludedPatches = summary.patchCounters[2];
    p.sampleLimitExceeded = summary.flags[4] != 0;
    p.valid = summary.flags[2] != 0;
    SurfaceSamplingLayout& layout = result.layout;
    const TapiocaSurfaceOptions& o = summary.options;
    layout.options = { static_cast<SamplingDomain> (o.domain), o.spacing, o.normalOffset, o.jitter, o.maxSamples };
    layout.valid = summary.flags[3] != 0;
    for (const TapiocaMeshSpan& pod : b.meshSpans)
        layout.meshes.push_back (MeshSpanOf (pod, b.masks));
    result.winding = { summary.winding[0], summary.winding[1], summary.winding[2], summary.winding[3] };
    result.reusedMeshes = summary.counts[0];
    result.rebuiltMeshes = summary.counts[1];
    result.reusedSamples = summary.counts[2];
    result.generatedSamples = summary.counts[3];
    result.valid = summary.flags[0] != 0;
    out = std::move (result);
    return true;
}

std::vector<TapiocaReuseMeshView> MeshViews (const geomsrv::Snapshot& snapshot)
{
    std::vector<TapiocaReuseMeshView> meshes;
    meshes.reserve (snapshot.meshes.size ());
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        meshes.push_back ({ { mesh.guid.data (), mesh.guid.size () },
                            { mesh.vertices.data (), mesh.vertices.size () },
                            { mesh.triangles.data (), mesh.triangles.size () } });
    return meshes;
}

TapiocaFaceLayout LayoutPod (const FaceLayout& layout)
{
    TapiocaFaceLayout pod {};
    std::copy (layout.origin, layout.origin + 3, pod.origin);
    std::copy (layout.uAxis, layout.uAxis + 3, pod.uAxis);
    std::copy (layout.vAxis, layout.vAxis + 3, pod.vAxis);
    pod.uStart = layout.uStart;
    pod.vStart = layout.vStart;
    pod.columns = layout.columns;
    pod.rows = layout.rows;
    pod.gridded = layout.gridded ? 1 : 0;
    return pod;
}

FaceLayout LayoutOf (const TapiocaFaceLayout& pod)
{
    FaceLayout layout;
    std::copy (pod.origin, pod.origin + 3, layout.origin);
    std::copy (pod.uAxis, pod.uAxis + 3, layout.uAxis);
    std::copy (pod.vAxis, pod.vAxis + 3, layout.vAxis);
    layout.uStart = static_cast<long> (pod.uStart);
    layout.vStart = static_cast<long> (pod.vStart);
    layout.columns = pod.columns;
    layout.rows = pod.rows;
    layout.gridded = pod.gridded != 0;
    return layout;
}

TapiocaPatchSpan SpanPod (const PatchSampleSpan& span)
{
    TapiocaPatchSpan pod {};
    pod.first = span.first;
    pod.count = span.count;
    pod.plane = span.key.plane;
    pod.occurrence = span.key.occurrence;
    pod.columns = span.columns;
    pod.rows = span.rows;
    std::copy (span.origin, span.origin + 3, pod.frame[0]);
    std::copy (span.uAxis, span.uAxis + 3, pod.frame[1]);
    std::copy (span.vAxis, span.vAxis + 3, pod.frame[2]);
    std::copy (span.normal, span.normal + 3, pod.frame[3]);
    std::copy (span.boundsMin, span.boundsMin + 3, pod.bounds[0]);
    std::copy (span.boundsMax, span.boundsMax + 3, pod.bounds[1]);
    pod.area = span.area;
    return pod;
}

} // namespace detail

bool BuildSurfaceSampling (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& sampleMask,
                           const SurfaceSamplingOptions& options, const StudyRecord* previous,
                           const std::function<bool ()>& isCancelled,
                           const std::vector<std::vector<uint8_t>>& faceMasks, SurfaceSamplingResult& out,
                           std::string& error)
{
    const auto meshes = MeshViews (snapshot);
    std::vector<TapiocaByteView> masks;
    masks.reserve (faceMasks.size ());
    for (const std::vector<uint8_t>& mask : faceMasks)
        masks.push_back ({ mask.data (), mask.size () });
    PreviousViews previousViews;
    const bool hasPrevious = previous != nullptr && previous->snapshot != nullptr;
    if (hasPrevious)
        Fill (previousViews, *previous);
    TapiocaSurfaceBuildRequest request {};
    request.meshes = { meshes.data (), meshes.size () };
    request.sampleMask = { sampleMask.data (), sampleMask.size () };
    request.faceMasks = { masks.data (), masks.size () };
    request.options = OptionsPod (options);
    request.previous = previousViews.view;
    request.cancel = { const_cast<std::function<bool ()>*> (&isCancelled), &PollCancelled };
    request.hasPrevious = hasPrevious ? 1 : 0;
    TapiocaSurfaceResult* raw = nullptr;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_surface_build (&request, &raw, m, n); }, error))
        return false;
    const ResultHandle handle (raw);
    return Read (handle.get (), out, error);
}

} // namespace evp::sunstudy::rust
