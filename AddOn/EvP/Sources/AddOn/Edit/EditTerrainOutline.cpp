#include "Edit/EditTerrainOutline.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditLandscapeInternal.hpp"
#include "Geometry/GeometryRustOutlineTerrain.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace tapioca::geometry {
namespace {
namespace edit = geomsrv::edit;
namespace cp = Clipper2Lib;
constexpr int kPrecision = landscape_detail::kPrecision;

cp::PathD Path (const std::vector<double>& ring)
{
    cp::PathD path;
    for (size_t i = 0; i + 2 < ring.size (); i += 3)
        path.push_back ({ ring[i], ring[i + 1] });
    return path;
}

// Strictly inside the area the paths fill: inside an outer ring and no hole, on none.
bool InsideShape (const cp::PathsD& shape, cp::PointD p)
{
    int winding = 0;
    for (const auto& path : shape) {
        const auto where = cp::PointInPolygon (p, path);
        if (where == cp::PointInPolygonResult::IsOn)
            return false;
        if (where == cp::PointInPolygonResult::IsInside)
            winding += cp::Area (path) > 0 ? 1 : -1;
    }
    return winding > 0;
}

// A closed ring as the two open halves a breakline must be, kept only when every vertex is
// strictly inside the region.
void AddRing (const cp::PathD& ring, const cp::PathD& region, std::vector<std::vector<double>>& lines)
{
    const size_t n = ring.size ();
    if (n < 3)
        return;
    for (const auto& p : ring)
        if (cp::PointInPolygon (p, region) != cp::PointInPolygonResult::IsInside)
            return;
    const size_t half = n / 2;
    std::vector<double> first, second;
    for (size_t i = 0; i <= half; ++i)
        first.insert (first.end (), { ring[i].x, ring[i].y, 0.0 });
    for (size_t i = half; i <= n; ++i)
        second.insert (second.end (), { ring[i % n].x, ring[i % n].y, 0.0 });
    lines.push_back (std::move (first));
    lines.push_back (std::move (second));
}

// The face lines' distances from the outline: four inside the rounding, then evenly to the reach.
std::vector<double> Distances (const OutlineDesignOptions& o)
{
    std::vector<double> d;
    if (o.rounding > 0)
        for (double k = 1; k <= 4; ++k)
            d.push_back (o.rounding * k / 4);
    const double step = std::max (0.5, o.limit / 12);
    for (double at = (d.empty () ? 0.0 : d.back ()) + step; at < o.limit + 1e-9; at += step)
        d.push_back (at);
    return d;
}
} // namespace

OutlineDesignOptions OutlineOptionsFor (const edit::Document& doc, edit::Id path, double limit)
{
    OutlineDesignOptions options;
    options.limit = limit;
    if (const auto* p = edit::FindPath (doc, path); p && !p->sections.empty ()) {
        const auto& s = p->sections.front ();
        options.cut = { TerrainSlope::Unit::Percent, s.cutSlope, 0 };
        options.fill = { TerrainSlope::Unit::Percent, s.fillSlope, 0 };
        options.rounding = s.rounding;
    }
    return options;
}

bool EvaluateOutlineDesign (const edit::Document& doc, const EditTerrainInput& input, const OutlineDesignOptions& o,
                            TerrainDesignResult& result, uint32_t& unreached, std::string& error)
{
    const auto paved = edit::Pave (doc.paths, o.pavedStep, true);
    if (paved.vertices.empty () || paved.triangles.empty ()) {
        error = "OutlineNoPaths: draw a path to grade the terrain";
        return false;
    }
    const auto areaRing = TerrainDesignArea (doc, input.mesh);
    if (areaRing.size () < 9) {
        error = "OutlineOutside: the terrain has no area inside the property line";
        return false;
    }
    const cp::PathD area = Path (areaRing);
    const auto inner = cp::InflatePaths ({ area }, -0.05, cp::JoinType::Miter, cp::EndType::Polygon, 2.0, kPrecision);
    cp::PathsD pavedShape;
    for (const auto& ring : paved.rings) {
        cp::PathD path;
        for (const auto& p : ring)
            path.push_back ({ p.x, p.y });
        pavedShape.push_back (std::move (path));
    }
    // The paved area where it may change the terrain.
    const auto graded = cp::Intersect (pavedShape, inner, cp::FillRule::NonZero, kPrecision);
    if (graded.empty ()) {
        error = "OutlineOutside: no path lies on the terrain inside the property line";
        return false;
    }
    // The region: the faces' reach and a margin around it, on the area; one piece.
    auto reach =
        cp::InflatePaths (graded, o.limit + o.margin, cp::JoinType::Round, cp::EndType::Polygon, 2.0, kPrecision, 0.05);
    reach = cp::Intersect (reach, { area }, cp::FillRule::NonZero, kPrecision);
    const auto largest = std::max_element (reach.begin (), reach.end (), [] (const cp::PathD& a, const cp::PathD& b) {
        return std::abs (cp::Area (a)) < std::abs (cp::Area (b));
    });
    if (largest == reach.end () || largest->size () < 3) {
        error = "OutlineOutside: the paths leave no region on the terrain";
        return false;
    }
    const cp::PathD region = *largest;
    // The outline and the rings its faces lie on, inside the region.
    std::vector<std::vector<double>> lines;
    for (const auto& ring : graded)
        AddRing (ring, region, lines);
    for (const double d : Distances (o)) {
        auto ring = cp::InflatePaths (graded, d, cp::JoinType::Round, cp::EndType::Polygon, 2.0, kPrecision, 0.05);
        ring = cp::Intersect (ring, inner, cp::FillRule::NonZero, kPrecision);
        for (const auto& path : ring)
            AddRing (path, region, lines);
    }
    // The paved surface's own vertices inside it, so its profile and crossfall are kept.
    std::vector<double> spots;
    for (const auto& v : paved.vertices)
        if (InsideShape (graded, { v.x, v.y }) &&
            cp::PointInPolygon (cp::PointD { v.x, v.y }, region) == cp::PointInPolygonResult::IsInside)
            spots.insert (spots.end (), { v.x, v.y, 0.0 });
    std::vector<double> surface;
    for (const auto& v : paved.vertices)
        surface.insert (surface.end (), { v.x, v.y, v.z });
    std::vector<double> regionRing;
    for (const auto& p : region)
        regionRing.insert (regionRing.end (), { p.x, p.y, 0.0 });
    OutlineTerrainRequest request;
    request.mesh = input.mesh;
    request.region = regionRing;
    request.surface = { surface, paved.triangles };
    request.lines = lines;
    request.spots = spots;
    request.cut = o.cut;
    request.fill = o.fill;
    request.limit = o.limit;
    request.rounding = o.rounding;
    request.tolerance = o.tolerance;
    request.effect = o.effect;
    OutlineTerrainResult built;
    if (!BuildOutlineTerrain (request, built, error))
        return false;
    TerrainDesignResult candidate;
    candidate.revision = input.revision;
    candidate.sourceTerrainGuid = input.sourceTerrainGuid;
    candidate.region = regionRing;
    auto& build = candidate.build;
    build.points = std::move (built.points);
    build.base = std::move (built.base);
    build.triangles = std::move (built.triangles);
    build.site = built.site;
    build.context = built.context;
    for (size_t i = 0; i < build.base.size (); ++i)
        build.owners.push_back (std::bit_cast<uint64_t> (build.points[i * 3 + 2]) !=
                                        std::bit_cast<uint64_t> (build.base[i])
                                    ? std::optional<uint32_t> (0)
                                    : std::nullopt);
    for (size_t i = 0; i < built.ring.size (); ++i)
        build.mappings.push_back ({ TerrainMappingKind::RingVertex, static_cast<uint32_t> (i), { built.ring[i] } });
    unreached = built.unreached;
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
