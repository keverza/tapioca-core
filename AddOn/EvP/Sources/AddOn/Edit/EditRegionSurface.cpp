#include "Edit/EditLandscape.hpp"
#include <cmath>

namespace tapioca::geometry {
bool ValidateRegionSurface (const RegionSurface& surface, std::string& error)
{
    if (surface.elevation > RegionElevationMode::Constant || surface.slope > RegionSlopeMode::ContourLine ||
        surface.fallsTo > RegionFallSide::Right || !geomsrv::edit::Finite (surface.first) ||
        !geomsrv::edit::Finite (surface.second) || !std::isfinite (surface.gradePercent) || surface.gradePercent < 0 ||
        surface.gradePercent > 1000) {
        error = "Region surface needs finite points, a valid mode and a grade from 0 to 1000 percent.";
        return false;
    }
    if (surface.elevation == RegionElevationMode::Existing && surface.slope != RegionSlopeMode::None) {
        error = "Choose Constant elevation before defining a target slope; Existing has no vertical target.";
        return false;
    }
    const double length = std::hypot (surface.second.x - surface.first.x, surface.second.y - surface.first.y);
    if (surface.slope != RegionSlopeMode::None &&
        (!std::isfinite (length) || length < 1e-6 || !std::isfinite ((surface.second.z - surface.first.z) / length))) {
        error = "A region slope needs two distinct points in plan (at least 1 micrometre apart).";
        return false;
    }
    return true;
}

std::optional<double> RegionSurfaceElevation (const RegionSurface& surface, Vec3 point)
{
    std::string error;
    if (!ValidateRegionSurface (surface, error) || surface.elevation == RegionElevationMode::Existing ||
        !geomsrv::edit::Finite (point))
        return std::nullopt;
    const double dx = surface.second.x - surface.first.x, dy = surface.second.y - surface.first.y;
    const double x = point.x - surface.first.x, y = point.y - surface.first.y;
    double z = surface.first.z;
    if (surface.slope == RegionSlopeMode::TwoPoints)
        z += (x * dx + y * dy) / (dx * dx + dy * dy) * (surface.second.z - surface.first.z);
    else if (surface.slope == RegionSlopeMode::ContourLine) {
        const double left = (-dy * x + dx * y) / std::hypot (dx, dy);
        z -= left * (surface.fallsTo == RegionFallSide::Left ? 1 : -1) * surface.gradePercent / 100;
    }
    return std::isfinite (z) ? std::optional<double> (z) : std::nullopt;
}
} // namespace tapioca::geometry
