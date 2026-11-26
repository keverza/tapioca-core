#pragma once

// A planting layer's daily light integral as one value per sample (#278
// Stage 5, #155): the figure a DLI map draws through the layer's own study
// (SunStudyStore::Represent). Plain values; traces nothing.
//
// ⚠️ A MISSING VALUE IS NOT ZERO LIGHT. A sample with no DLI for the picked
// month (no weather that month, or an excluded sample) is counted in
// `missing` and drawn as 0, because a presented value must be finite. The
// caller says so beside the map.

#include <array>
#include <string>
#include <vector>

namespace evp::sunstudy {

struct DliMap {
    std::vector<double> values; // mol/m2/day per sample, 0 where missing
    double top = 0.0;           // the ramp's top, mol/m2/day: a step of 5 at or above the largest value
    double largest = 0.0;
    size_t missing = 0;
};

// `month` 1-12, or 0 for the mean over the months a sample has weather for.
// Refused for a month outside 0-12 or when no sample has a value.
bool BuildDliMap (const std::vector<std::array<double, 12>>& dli, int month, DliMap& out, std::string& error);

// The ramp's top for a largest value: the next multiple of 5 mol/m2/day at or
// above it, at least 5.
double DliRampTop (double largest);

} // namespace evp::sunstudy
