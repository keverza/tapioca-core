#ifndef EVP_NATIVECOMMANDS_SUNSTUDYSEASONAL_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYSEASONAL_HPP

#include "NativeCommands/SunStudyHudRunner.hpp"

#include <array>

namespace geomsrv::sunstudyhud {

struct SeasonalPointDay {
    std::string label;  // DJF / MAM / JJA / SON: stable id
    std::string name;   // Winter / Spring / Summer / Autumn for the project's hemisphere
    std::string months; // "Dec-Feb" ...
    evp::sunstudy::PointDay day;
    size_t tracedDays = 0;
    double representedDays = 0.0; // sum of retained traced-day weights
};

// Display-only meteorological-season clocks from the retained point profile.
// Preview weights apply to daily/clock means, never to fabricated extra frames.
// The hemisphere is read from the measured sun, not from a solar formula: the
// season whose mean sun-up hours are longer of DJF and JJA is the summer.
std::array<SeasonalPointDay, 4> SeasonalPointDays (const PeriodProfile& profile);

} // namespace geomsrv::sunstudyhud

#endif
