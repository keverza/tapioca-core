#ifndef EVP_SUNSTUDY_SUNSTUDYLIGHTCLASS_HPP
#define EVP_SUNSTUDY_SUNSTUDYLIGHTCLASS_HPP

// SunStudy/SunStudyLightClass -- area-weighted sunlight classes (#278).
//
// With edges { 3, 6 }: H < 3 shade, 3 <= H < 6 partial shade, H >= 6 full sun.
// A sample's class is the number of edges it reaches, so an exact edge value
// belongs to the upper class. The kernel is Rust `light_class`.
//
// ⚠️ NOT THE DISPLAY BAND. The tint shader draws a value a rounding error short
// of an edge in the upper band (kSunBandSlack); classes, inspector labels and
// exports come from the raw hours through this comparator instead.
//
// ⚠️ AREA, NOT COUNTS. Shares are summed weight over summed weight.

#include <cstddef>
#include <string>
#include <vector>

namespace evp::sunstudy {

struct LightClassAreas {
    std::vector<double> areas;  // per class, lowest first (edges + 1 classes)
    std::vector<size_t> counts; // samples per class
    double evaluatedArea = 0.0;
    size_t unclassified = 0; // non-finite hours/weight, or negative weight
};

// False, with `error`, for mismatched arrays or edges that are empty, more than
// 16, non-finite or not strictly ascending; `out` is then empty.
bool ClassifyLightAreas (const std::vector<double>& hours, const std::vector<double>& areas,
                         const std::vector<double>& edges, LightClassAreas& out, std::string& error);

} // namespace evp::sunstudy

#endif
