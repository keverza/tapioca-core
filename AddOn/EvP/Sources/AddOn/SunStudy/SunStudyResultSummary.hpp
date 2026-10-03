#ifndef EVP_SUNSTUDY_RESULTSUMMARY_HPP
#define EVP_SUNSTUDY_RESULTSUMMARY_HPP

#include <algorithm>
#include <cstddef>
#include <vector>

namespace evp::sunstudy {

struct SunStudyResultSummary {
    size_t count = 0, fullyLit = 0, fullyShaded = 0;
    double minHours = 0.0, meanHours = 0.0, maxHours = 0.0, daylightHours = 0.0;
};

template <typename HoursAt>
SunStudyResultSummary SummarizeSunHours (size_t count, double daylightHours, const HoursAt& hoursAt)
{
    SunStudyResultSummary summary;
    summary.count = count;
    summary.daylightHours = daylightHours;
    if (count == 0)
        return summary;
    summary.minHours = summary.maxHours = hoursAt (0);
    double sum = 0.0;
    for (size_t sample = 0; sample < count; ++sample) {
        const double value = hoursAt (sample);
        summary.minHours = std::min (summary.minHours, value);
        summary.maxHours = std::max (summary.maxHours, value);
        sum += value;
        summary.fullyLit += value >= daylightHours - 1e-9 ? 1 : 0;
        summary.fullyShaded += value <= 1e-9 ? 1 : 0;
    }
    summary.meanHours = sum / count;
    return summary;
}

inline SunStudyResultSummary SummarizeSunHours (const std::vector<double>& hours, double daylightHours)
{
    return SummarizeSunHours (hours.size (), daylightHours, [&hours] (size_t sample) { return hours[sample]; });
}

} // namespace evp::sunstudy
#endif
