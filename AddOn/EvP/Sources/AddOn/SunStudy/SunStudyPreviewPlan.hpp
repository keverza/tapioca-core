#ifndef EVP_SUNSTUDY_SUNSTUDYPREVIEWPLAN_HPP
#define EVP_SUNSTUDY_SUNSTUDYPREVIEWPLAN_HPP

#include <cmath>
#include <cstddef>

namespace evp::sunstudy {

struct SunStudyPreviewPlan {
    bool enabled = false;
    double previewSpacing = 0.0;
    double requestedSpacing = 0.0;
};

inline SunStudyPreviewPlan MakeSunStudyPreviewPlan (size_t samples, double requestedSpacing)
{
    SunStudyPreviewPlan plan;
    plan.requestedSpacing = requestedSpacing;
    plan.previewSpacing = requestedSpacing;
    // Avoid doubling setup work for small runs. Only space is coarsened: dates,
    // all sun timesteps, roles, occluders and the final requested grid stay fixed.
    if (samples >= 65536 && requestedSpacing > 0.0 && std::isfinite (requestedSpacing * 4.0)) {
        plan.enabled = true;
        plan.previewSpacing = requestedSpacing * 4.0;
    }
    return plan;
}

} // namespace evp::sunstudy

#endif
