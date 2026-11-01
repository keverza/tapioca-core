#ifndef TAPIOCA_ARCHVIZ_HUDSLOPEFIELDS_HPP
#define TAPIOCA_ARCHVIZ_HUDSLOPEFIELDS_HPP

#include "Geometry/GeometryRustTerrain.hpp"
#include <imgui.h>
#include <algorithm>
#include <limits>
#include <numbers>

// OVERLAY-INVARIANTS.md §§8,14: presentation only; callers retain session,
// history and slider-gesture ownership. No renderer or native-write lifecycle.
namespace geomsrv::archviz::edithud {
struct SlopeFieldLabels {
    const char *unit, *percent, *angle, *rise, *run, *height, *planRun, *hint;
    bool allowZero = false;
};

template <class Slider>
bool SlopeFields (const SlopeFieldLabels& labels, tapioca::geometry::TerrainSlope& slope, std::string& error,
                  Slider slider)
{
    namespace lg = tapioca::geometry;
    using Unit = lg::TerrainSlope::Unit;
    int unit = static_cast<int> (slope.unit) - 1;
    bool changed = false;
    if (ImGui::Combo (labels.unit, &unit, "Percent (%)\0Angle (degrees)\0Rise:run\0Height / run (m)\0")) {
        lg::TerrainSlope converted;
        if (lg::ConvertTerrainSlope (slope, static_cast<Unit> (unit + 1), 1, converted, error)) {
            slope = converted;
            changed = true;
        }
    }
    if (slope.unit == Unit::Percent)
        changed |= slider (labels.percent, slope.a, labels.allowZero ? 0 : 0.01, 1000, "%.2f");
    else if (slope.unit == Unit::Angle) {
        // UI boundary only: the authored angle and Rust ABI remain radians.
        constexpr double degrees = 180.0 / std::numbers::pi;
        double angle = slope.a * degrees;
        if (slider (labels.angle, angle, labels.allowZero ? 0 : 0.01, 90, "%.3f")) {
            slope.a = angle / degrees;
            changed = true;
        }
    }
    else {
        const bool height = slope.unit == Unit::Height;
        const auto upper = [] (double value) {
            const double maximum = std::numeric_limits<double>::max ();
            return std::max (10.0, value <= maximum / 2 ? value * 2 : maximum);
        };
        changed |= slider (height ? labels.height : labels.rise, slope.a, labels.allowZero ? 0 : 0.0001,
                           upper (slope.a), "%.4f");
        changed |= slider (height ? labels.planRun : labels.run, slope.b, 0.0001, upper (slope.b), "%.4f");
    }
    ImGui::TextDisabled ("%s", labels.hint);
    return changed;
}
} // namespace geomsrv::archviz::edithud
#endif
