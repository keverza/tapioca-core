#ifndef EVP_NATIVECOMMANDS_SUNSTUDYSEASONAL_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYSEASONAL_HPP

#include "NativeCommands/SunStudyHudRunner.hpp"

#include <array>

namespace geomsrv::sunstudyhud {

struct SeasonalPointDay {
    std::string label; // DJF / MAM / JJA / SON
    evp::sunstudy::PointDay day;
    size_t tracedDays = 0;
    double representedDays = 0.0; // sum of retained traced-day weights
};

// Display-only meteorological-season clocks from the retained point profile.
// Preview weights apply to daily/clock means, never to fabricated extra frames.
std::array<SeasonalPointDay, 4> SeasonalPointDays (const PeriodProfile& profile);

} // namespace geomsrv::sunstudyhud

#endif
