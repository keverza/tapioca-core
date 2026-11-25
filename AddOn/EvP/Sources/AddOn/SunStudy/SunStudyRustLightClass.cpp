#include "SunStudy/SunStudyLightClass.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

namespace evp::sunstudy {

using namespace rust::detail;

bool ClassifyLightAreas (const std::vector<double>& hours, const std::vector<double>& areas,
                         const std::vector<double>& edges, LightClassAreas& out, std::string& error)
{
    out = LightClassAreas ();
    const TapiocaClassAreasRequest request { { hours.data (), hours.size () },
                                             { areas.data (), areas.size () },
                                             { edges.data (), edges.size () } };
    LightClassAreas result;
    result.areas.resize (edges.size () + 1);
    result.counts.resize (edges.size () + 1);
    TapiocaClassAreasSummary summary {};
    const TapiocaClassAreasOutput output { result.areas.data (), result.counts.data (), result.areas.size (),
                                           &summary };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_class_areas (&request, &output, m, n); }, error))
        return false;
    result.evaluatedArea = summary.evaluatedArea;
    result.unclassified = summary.unclassified;
    out = std::move (result);
    return true;
}

} // namespace evp::sunstudy
