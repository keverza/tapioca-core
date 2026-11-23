#ifndef TAPIOCA_SUNSTUDY_ANALYSISROI_HPP
#define TAPIOCA_SUNSTUDY_ANALYSISROI_HPP

#include <cmath>

namespace tapioca::sunstudy {

// World-metre plan envelope for framing. This rectangle does not determine the
// measured area (a cylinder, for example, occupies only part of its envelope).
struct AnalysisRoi {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;

    bool IsValid () const
    {
        return std::isfinite (minX) && std::isfinite (minY) && std::isfinite (maxX) && std::isfinite (maxY) &&
               minX < maxX && minY < maxY;
    }
};

} // namespace tapioca::sunstudy

#endif
