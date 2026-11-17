#include "Edit/EditTerrainDetail.hpp"
#include "Geometry/GeometryRustHybridTerrain.hpp"
#include "Geometry/GeometryRustTerrain.hpp"
#include <bit>
#include <cmath>
#include <set>
#include <utility>

namespace tapioca::geometry {

DetailFeatures PublishedDetailFeatures (const PublishedTerrain& published, const std::vector<double>& baselinePoints)
{
    DetailFeatures features;
    const size_t count = published.points.size () / 3;
    std::vector<bool> kept (count, false);
    // The existing ground's own vertices, where the design left their height.
    std::set<std::pair<uint64_t, uint64_t>> native;
    for (size_t i = 0; i + 2 < baselinePoints.size (); i += 3)
        native.emplace (std::bit_cast<uint64_t> (baselinePoints[i]), std::bit_cast<uint64_t> (baselinePoints[i + 1]));
    for (size_t v = 0; v < count && v < published.base.size (); ++v) {
        const double x = published.points[3 * v], y = published.points[3 * v + 1], z = published.points[3 * v + 2];
        if (std::bit_cast<uint64_t> (z) == std::bit_cast<uint64_t> (published.base[v]) &&
            native.contains ({ std::bit_cast<uint64_t> (x), std::bit_cast<uint64_t> (y) }))
            kept[v] = true;
    }
    // Fixed-height controls.
    for (const auto& locked : published.locked)
        if (locked.vertex < count)
            kept[locked.vertex] = true;
    for (uint32_t v = 0; v < count; ++v)
        if (kept[v])
            features.keep.push_back (v);
    // Authored lines: the paths' design lines (16) and the split's line edges (11: regions,
    // property).
    const auto edge = [&] (uint32_t a, uint32_t b) {
        if (a < count && b < count && a != b)
            features.lines.insert (features.lines.end (), { a, b });
    };
    // A path's own lines are authored: its band edges, shoulders, ends, crown and fixed controls.
    // Its daylight (where a slope happens to meet the ground), sampled sections and face lines are
    // derived, and are kept only as far as the tolerance needs them.
    // The outline lane's lines (domain Outline) are the paved outline the caller gave (part 0),
    // then the pavement's private supports (1) and the derived toe and creases: only the first
    // is authored.
    const auto authored = [&] (uint32_t line) {
        if (line >= published.lines.size ())
            return true;
        const auto& origin = published.lines[line];
        if (origin.domain == UnifiedLineDomain::Outline)
            return origin.part == 0;
        if (!origin.kind)
            return true; // a region's, mask's or control's line
        switch (*origin.kind) {
            case HybridLineKind::Daylight:
            case HybridLineKind::Section:
            case HybridLineKind::Face:
                return false;
            default:
                return true;
        }
    };
    for (const auto& row : published.mappings) {
        if (row.kind == TerrainMappingKind::DesignLineEdge && authored (row.index))
            edge (row.values[1], row.values[2]);
        else if (row.kind == TerrainMappingKind::LineEdge)
            edge (row.values[0], row.values[1]);
    }
    return features;
}

bool DetailPublishedTerrain (const PublishedTerrain& published, const std::vector<double>& baselinePoints,
                             double tolerance, DetailedTerrain& detailed, std::string& error)
{
    if (!(tolerance > 0.0) || !std::isfinite (tolerance)) {
        error = "MeshDetail: the tolerance must be a positive distance";
        return false;
    }
    const auto features = PublishedDetailFeatures (published, baselinePoints);
    TerrainResult result;
    if (!DetailTerrainSurface ({ published.points, published.triangles, features.keep, features.lines, tolerance },
                               result, error))
        return false;
    DetailedTerrain out;
    out.points = std::move (result.points);
    out.triangles = std::move (result.triangles);
    out.source.assign (out.points.size () / 3, UINT32_MAX);
    for (const auto& row : result.mappings) {
        if (row.kind == TerrainMappingKind::DetailVertex && row.index < out.source.size ())
            out.source[row.index] = row.values[0];
        else if (row.kind == TerrainMappingKind::DetailLineEdge)
            out.lines.push_back ({ row.values[0], row.values[1] });
    }
    out.tolerance = tolerance;
    out.revision = published.revision;
    out.builtTriangles = published.triangles.size () / 3;
    detailed = std::move (out);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
