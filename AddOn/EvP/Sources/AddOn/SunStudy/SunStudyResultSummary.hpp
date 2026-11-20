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

} // namespace evp::sunstudy
#endif
