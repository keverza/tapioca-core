#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"
#include "SunStudy/SunStudyStore.hpp"

namespace evp::sunstudy::rust {

using namespace detail;

namespace {

// Views over one record; valid while the record lives.
struct StudyViews {
    std::vector<TapiocaReuseMeshView> meshes;
    std::vector<TapiocaSunStep> steps;
    TapiocaReuseStudyView view {};
};

void Fill (StudyViews& views, const StudyRecord& record)
{
    views.meshes = MeshViews (*record.snapshot);
    for (const SunStep& step : record.series.Steps ())
        views.steps.push_back (ToPod (step));
    TapiocaReuseStudyView& v = views.view;
    v.meshes = views.meshes.data ();
    v.meshCount = views.meshes.size ();
    v.roles = { record.elementRoles.data (), record.elementRoles.size () };
    v.positions = { record.positions.data (), record.positions.size () };
    v.normals = { record.normals.data (), record.normals.size () };
    v.sampleMeshes = { record.sampleMeshes.data (), record.sampleMeshes.size () };
    v.series = { { views.steps.data (), views.steps.size () },
                 record.series.TimestepMinutes (),
                 record.series.SourceStepCount (),
                 record.series.Version () };
    v.gridSpacing = record.gridSpacing;
    v.domain = static_cast<uint8_t> (record.domain);
}

uint8_t PollCancelled (void* context)
{
    try {
        return static_cast<const std::atomic<bool>*> (context)->load () ? 1 : 0;
    }
    catch (...) {
        return 1;
    }
}

} // namespace

size_t ReuseUnaffectedSamples (const StudyRecord& source, StudyRecord& target, const std::atomic<bool>& cancelled,
                               std::string& error)
{
    error.clear ();
    if (source.snapshot == nullptr || target.snapshot == nullptr)
        return 0;
    StudyViews sourceViews, targetViews;
    Fill (sourceViews, source);
    Fill (targetViews, target);
    TapiocaReusePlanRequest request {};
    request.source = sourceViews.view;
    request.target = targetViews.view;
    request.cancel = { const_cast<std::atomic<bool>*> (&cancelled), &PollCancelled };
    request.sourceConverged = source.session.Progress ().converged ? 1 : 0;
    request.sourceDefaultRayBounds = source.defaultRayBounds ? 1 : 0;
    const size_t samples = target.sampleMeshes.size ();
    const size_t words = (target.series.StepCount () + 63) / 64;
    std::vector<size_t> map (samples);
    std::vector<uint64_t> dirty (samples * words);
    TapiocaReusePlanSummary summary {};
    const TapiocaReusePlanOutput output { map.data (), map.size (), dirty.data (), dirty.size (), &summary };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_plan_reuse (&request, &output, m, n); }, error))
        return 0;
    if (summary.planned == 0 || cancelled.load () ||
        !target.session.SeedReusable (source.session.Accumulator (), map, dirty))
        return 0;
    target.reusedSamples = summary.reusedSamples;
    target.reusedSampleSteps = summary.reusedSampleSteps;
    return summary.reusedSamples;
}

bool SetSampleMeshes (StudyRecord& record, std::string& error)
{
    error.clear ();
    if (!record.IsPatchDomain ()) {
        record.sampleMeshes = record.sampleGrid.groups;
        return true;
    }
    if (record.snapshot == nullptr)
        return true;
    const auto meshes = MeshViews (*record.snapshot);
    std::vector<TapiocaSpanOwnerView> spans;
    spans.reserve (record.patchGrid.spans.size ());
    for (const PatchSampleSpan& span : record.patchGrid.spans)
        spans.push_back ({ { span.key.element.data (), span.key.element.size () }, span.first, span.count });
    const size_t samples = record.positions.size () / 3;
    const TapiocaSampleMeshRequest request { meshes.data (), meshes.size (), spans.data (), spans.size (), samples };
    std::vector<uint32_t> out (samples);
    uint8_t outcome = 0;
    const TapiocaSampleMeshOutput output { out.data (), out.size (), &outcome };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_patch_sample_meshes (&request, &output, m, n); },
                 error))
        return false;
    if (outcome == 0)
        record.sampleMeshes = std::move (out);
    else if (outcome == 2)
        record.sampleMeshes.clear (); // a span names a missing mesh or overruns
    // outcome 1 (ambiguous meshes) leaves the map as it was, like C++.
    return true;
}

bool SummarizeSunHours (const std::vector<double>& hours, double daylightHours, SunStudyResultSummary& out,
                        std::string& error)
{
    const TapiocaSummaryRequest request { { hours.data (), hours.size () }, daylightHours };
    TapiocaResultSummary summary {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_summarize (&request, &summary, m, n); }, error))
        return false;
    out.count = summary.count;
    out.fullyLit = summary.fullyLit;
    out.fullyShaded = summary.fullyShaded;
    out.minHours = summary.minHours;
    out.meanHours = summary.meanHours;
    out.maxHours = summary.maxHours;
    out.daylightHours = summary.daylightHours;
    return true;
}

} // namespace evp::sunstudy::rust
