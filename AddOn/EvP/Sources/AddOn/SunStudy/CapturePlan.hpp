#ifndef TAPIOCA_SUNSTUDY_CAPTUREPLAN_HPP
#define TAPIOCA_SUNSTUDY_CAPTUREPLAN_HPP

#include "SunStudy/AnalysisRoi.hpp"
#include <array>
#include <cstdint>

namespace tapioca::sunstudy {
// Model +Y at the top. Zero is background; nonzero cells encode the
// relative roof height, not a sun measurement.
struct CapturePlan {
    static constexpr int side = 32;
    std::array<uint8_t, side * side> cells {};
};
} // namespace tapioca::sunstudy
#endif
