#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"
#include "SunStudy/SunStudyStore.hpp"

namespace evp::sunstudy::rust {

using namespace detail;

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
