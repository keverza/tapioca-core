#include "Edit/EditLandscape.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathNetwork.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace tapioca::geometry {
namespace {
constexpr const char* s_terrainError =
    "RegionTerrain: finite transition widths/reaches (0.05..100 m) and positive cut/fill grades (<=1000%) "
    "required; vertical daylight faces are not supported by these fields";

bool ResolveFiniteGrade (const TerrainSlope& authored, bool allowZero, TerrainSlope& resolved, std::string& error)
{
    auto percent = authored;
    // Percent needs only policy checks (also keeps legacy documents readable in
    // NoRust builds). All other unit conversion belongs to the Rust slope service.
    if (authored.unit != TerrainSlope::Unit::Percent &&
        !ConvertTerrainSlope (authored, TerrainSlope::Unit::Percent, 1, percent, error))
        return false;
    // atan(10) -> tan -> percent can cross 1000 by roundoff. Permit only
    // 16 f64 eps relative at this converted-unit policy boundary; never clamp
    // the authored grade or relax the exact percent-input limit.
    const double limit = authored.unit == TerrainSlope::Unit::Percent
                             ? 1000.0
                             : 1000.0 * (1 + 16 * std::numeric_limits<double>::epsilon ());
    if (!std::isfinite (percent.a) || percent.a < 0 || (!allowZero && percent.a == 0) || percent.a > limit ||
        percent.b != 0) {
        error = allowZero ? "Region surface grade must be finite and from 0 to 1000 percent; vertical is unsupported."
                          : s_terrainError;
        return false;
    }
    resolved = percent;
    return true;
}
} // namespace

bool ValidateRegionTerrain (const HardscapeObject& object, std::string& error)
{
    const auto& terrain = object.terrain;
    TerrainSlope percent;
    if (!ResolveFiniteGrade (terrain.cut, false, percent, error) ||
        !ResolveFiniteGrade (terrain.fill, false, percent, error))
        return false;
    if (terrain.effect > RegionTerrainEffect::Subtract || terrain.transition > RegionTerrainTransition::Daylight ||
        !std::isfinite (terrain.falloff) || terrain.falloff < 0.05 || terrain.falloff > 100 ||
        !std::isfinite (terrain.reach) || terrain.reach < 0.05 || terrain.reach > 100) {
        error = s_terrainError;
        return false;
    }
    // #112 P1: the durable owner and its root operation are the identity; a live
    // boundary source is required only to resolve Boundary curve Z (SetRegionSurface
    // gates that mode), not to enable a vertical effect on a derived domain.
    if (terrain.effect != RegionTerrainEffect::None &&
        (!object.surface || object.surface->elevation == RegionElevationMode::Existing)) {
        error = "RegionTerrain: choose Boundary curve Z or Constant elevation before enabling Grade/Add/Subtract";
        return false;
    }
    return true;
}
bool PrepareRegionSurface (const RegionSurface& surface, PreparedRegionSurface& prepared, std::string& error)
{
    TerrainSlope percent;
    if (!ResolveFiniteGrade (surface.grade, true, percent, error))
        return false;
    if (surface.elevation > RegionElevationMode::Boundary || surface.slope > RegionSlopeMode::ContourLine ||
        surface.fallsTo > RegionFallSide::Right || !geomsrv::edit::Finite (surface.first) ||
        !geomsrv::edit::Finite (surface.second)) {
        error = "Region surface needs finite points and a valid mode.";
        return false;
    }
    if (surface.elevation != RegionElevationMode::Constant && surface.slope != RegionSlopeMode::None) {
        error = "Explicit slopes require Constant elevation; Boundary uses the curve Z and Existing has no vertical "
                "target.";
        return false;
    }
    const double length = std::hypot (surface.second.x - surface.first.x, surface.second.y - surface.first.y);
    if (surface.slope != RegionSlopeMode::None &&
        (!std::isfinite (length) || length < 1e-6 || !std::isfinite ((surface.second.z - surface.first.z) / length))) {
        error = "A region slope needs two distinct points in plan (at least 1 micrometre apart).";
        return false;
    }
    PreparedRegionSurface candidate;
    candidate.surface = surface;
    candidate.gradePercent = percent.a;
    prepared = candidate;
    return true;
}

bool ValidateRegionSurface (const RegionSurface& surface, std::string& error)
{
    PreparedRegionSurface prepared;
    return PrepareRegionSurface (surface, prepared, error);
}

std::optional<double> RegionSurfaceElevation (const RegionSurface& authored, Vec3 point)
{
    std::string error;
    PreparedRegionSurface prepared;
    return PrepareRegionSurface (authored, prepared, error) ? prepared.Elevation (point) : std::nullopt;
}

std::optional<double> PreparedRegionSurface::Elevation (Vec3 point) const
{
    if (surface.elevation != RegionElevationMode::Constant || !geomsrv::edit::Finite (point))
        return std::nullopt;
    const double dx = surface.second.x - surface.first.x, dy = surface.second.y - surface.first.y;
    const double x = point.x - surface.first.x, y = point.y - surface.first.y;
    double z = surface.first.z;
    if (surface.slope == RegionSlopeMode::TwoPoints)
        z += (x * dx + y * dy) / (dx * dx + dy * dy) * (surface.second.z - surface.first.z);
    else if (surface.slope == RegionSlopeMode::ContourLine) {
        const double left = (-dy * x + dx * y) / std::hypot (dx, dy);
        z -= left * (surface.fallsTo == RegionFallSide::Left ? 1 : -1) * gradePercent / 100;
    }
    return std::isfinite (z) ? std::optional<double> (z) : std::nullopt;
}

bool BuildRegionSurfaceMesh (const geomsrv::edit::Document& doc, const HardscapeObject& object,
                             const DerivedRegion& footprint, LandscapePreviewMesh& mesh, std::string& error)
{
    error.clear ();
    if (!object.surface || object.surface->elevation == RegionElevationMode::Existing) {
        error = "RegionSurface: an authored elevation target is required";
        return false;
    }
    PreparedRegionSurface prepared;
    if (!PrepareRegionSurface (*object.surface, prepared, error))
        return false;
    LandscapePreviewMesh target;
    if (!BuildLandscapePreviewMesh (footprint, target, error))
        return false;
    if (object.surface->elevation == RegionElevationMode::Constant) {
        for (auto& point : target.vertices) {
            const auto z = prepared.Elevation (point);
            if (!z) {
                error = "RegionSurface: nonfinite target height";
                return false;
            }
            point.z = *z;
        }
    }
    else {
        if (!doc.landscape) {
            error = "RegionSurface: landscape is missing";
            return false;
        }
        const auto source =
            std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (), [&] (const auto& s) {
                return s.id == object.boundarySource && !s.deleted && s.role == SourceRole::RegionBoundary;
            });
        std::vector<Vec3> boundary;
        bool closed = false;
        if (source == doc.landscape->sources.end () ||
            !LandscapeSourcePreview (doc, *source, boundary, closed, error) || !closed) {
            if (error.empty ())
                error = "RegionSurface: live closed boundary required";
            return false;
        }
        DerivedRegion authored;
        authored.rings = { boundary };
        LandscapePreviewMesh cover;
        if (!BuildLandscapePreviewMesh (authored, cover, error))
            return false;
        // The existing polygon triangulator retains boundary vertex order. Use
        // the actual sampled curve heights, never the flattened DAG's seed Z.
        cover.vertices = boundary;
        geomsrv::edit::PavedArea surface;
        surface.vertices = std::move (cover.vertices);
        surface.triangles = std::move (cover.triangles);
        for (auto& point : target.vertices)
            if (!geomsrv::edit::PavedHeight (surface, point, point.z)) {
                error =
                    "RegionSurface: derived footprint leaves its authored height surface; choose a Constant override";
                return false;
            }
    }
    mesh = std::move (target);
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
