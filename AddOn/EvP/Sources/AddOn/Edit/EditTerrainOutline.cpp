#include "Edit/EditTerrainOutline.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditPath.hpp"
#include "Geometry/GeometryRustTerrain.hpp"
#include "Edit/EditLandscapeInternal.hpp"
#include "Geometry/GeometryRustOutlineTerrain.hpp"
#include "Geometry/ClipperPlanRing.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>

namespace tapioca::geometry {
namespace {
namespace edit = geomsrv::edit;
namespace cp = Clipper2Lib;
constexpr int kPrecision = landscape_detail::kPrecision;
// How far the paved surface the terrain is graded from may stray from the paths' heights, metres:
// its triangles split only where they do (Pave), so a long level or evenly graded path is a few
// long triangles rather than one every half metre, and the faces, supports and region rebuilt from
// them scale with the shape, not the length.
constexpr double kPavedTolerance = 0.002;

cp::PathD Path (const std::vector<double>& ring)
{
    cp::PathD path;
    for (size_t i = 0; i + 2 < ring.size (); i += 3)
        path.push_back ({ ring[i], ring[i + 1] });
    return path;
}

// Strictly inside the area the paths fill: inside an outer ring and no hole, on none.
bool InsideShape (const std::vector<std::pair<ClipperPlanRing, int>>& shape, cp::PointD p)
{
    int winding = 0;
    for (const auto& [path, sign] : shape) {
        const auto where = path.Locate (p);
        if (where == cp::PointInPolygonResult::IsOn)
            return false;
        if (where == cp::PointInPolygonResult::IsInside)
            winding += sign;
    }
    return winding > 0;
}

// `source` with no side longer than `spacing` (0 keeps it): a face's height is sampled along
// its ring as well as across the rings, or a long side's two ends span the toe.
cp::PathD Densify (const cp::PathD& source, bool closed, double spacing)
{
    cp::PathD path;
    const size_t sides = closed ? source.size () : source.size () - 1;
    for (size_t i = 0; i < sides; ++i) {
        const auto& a = source[i];
        const auto& b = source[(i + 1) % source.size ()];
        path.push_back (a);
        const double pieces = spacing > 0 ? std::ceil (std::hypot (b.x - a.x, b.y - a.y) / spacing) : 1;
        for (double k = 1; k < pieces; ++k)
            path.push_back ({ a.x + (b.x - a.x) * k / pieces, a.y + (b.y - a.y) * k / pieces });
    }
    if (!closed && !source.empty ())
        path.push_back (source.back ());
    return path;
}

// A closed ring as the two open halves a breakline must be, kept only when every vertex is
// strictly inside the region.
void AddRing (const cp::PathD& source, const ClipperPlanRing& region, std::vector<std::vector<double>>& lines,
              double spacing = 0)
{
    const auto ring = Densify (source, true, spacing);
    const size_t n = ring.size ();
    if (n < 3)
        return;
    for (const auto& p : ring)
        if (region.Locate (p) != cp::PointInPolygonResult::IsInside)
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

// An open piece of a ring as one breakline, kept only when every vertex is strictly inside
// the region and it is longer than the area's edge margin.
void AddLine (const cp::PathD& source, const ClipperPlanRing& region, std::vector<std::vector<double>>& lines,
              double spacing)
{
    if (source.size () < 2)
        return;
    double length = 0;
    for (size_t i = 1; i < source.size (); ++i)
        length += std::hypot (source[i].x - source[i - 1].x, source[i].y - source[i - 1].y);
    if (length < 0.05)
        return;
    std::vector<double> line;
    for (const auto& p : Densify (source, false, spacing)) {
        if (region.Locate (p) != cp::PointInPolygonResult::IsInside)
            return;
        line.insert (line.end (), { p.x, p.y, 0.0 });
    }
    lines.push_back (std::move (line));
}

// The rings' spacing beyond the rounding, metres: half a metre up to a 10 m reach, then a
// twentieth of it. The toe falls between two rings, so this bounds how far it is interpolated.
double RingStep (const OutlineDesignOptions& o)
{
    return std::max (0.5, o.limit / 20);
}

// Per paved vertex, the faces of the path section it was paved from: the path whose centreline
// is nearest in plan, its range in force at the station there (#151). A junction or a kerb
// fillet takes its nearest path's. Slopes become gradients through the kernel's own conversion;
// one that does not convert grades no face. Empty when no path has an alignment.
std::vector<double> SurfaceGrades (const edit::Document& doc, const std::vector<edit::Vec3>& vertices)
{
    struct Aligned {
        const edit::Path* path;
        edit::AlignmentGeometry geometry;
    };
    std::vector<Aligned> aligned;
    for (const auto& path : doc.paths) {
        Aligned entry { &path, {} };
        std::string ignored;
        if (!path.sections.empty () && edit::BuildAlignment (path.points, entry.geometry, ignored))
            aligned.push_back (std::move (entry));
    }
    if (aligned.empty ())
        return {};
    const auto gradient = [] (const TerrainSlope& slope) {
        TerrainSlope percent;
        std::string ignored;
        return ConvertTerrainSlope (slope, TerrainSlope::Unit::Percent, 1.0, percent, ignored) &&
                       std::isfinite (percent.a) && percent.a >= 0
                   ? percent.a / 100
                   : std::numeric_limits<double>::infinity ();
    };
    std::map<const edit::Section*, std::array<double, 3>> converted;
    std::vector<double> grades;
    grades.reserve (vertices.size () * 3);
    for (const auto& v : vertices) {
        const edit::Section* best = nullptr;
        double nearest = std::numeric_limits<double>::infinity ();
        for (const auto& [path, geometry] : aligned) {
            double station = 0, offset = 0;
            edit::Placement at;
            if (!edit::StationOf (geometry, { v.x, v.y, 0 }, station, offset) || !edit::Locate (geometry, station, at))
                continue;
            const double distance = std::hypot (at.point.x - v.x, at.point.y - v.y);
            if (const auto* section = edit::SectionAt (path->sections, station); section && distance < nearest) {
                nearest = distance;
                best = section;
            }
        }
        if (!best)
            best = &aligned.front ().path->sections.front ();
        auto found = converted.find (best);
        if (found == converted.end ())
            found = converted
                        .emplace (best, std::array { gradient (best->cutSlope), gradient (best->fillSlope),
                                                     std::max (0.0, best->rounding) })
                        .first;
        grades.insert (grades.end (), found->second.begin (), found->second.end ());
    }
    return grades;
}

// The face lines' distances from the outline: four inside the rounding, then evenly to the reach.
std::vector<double> Distances (const OutlineDesignOptions& o)
{
    std::vector<double> d;
    if (o.rounding > 0)
        for (double k = 1; k <= 4; ++k)
            d.push_back (o.rounding * k / 4);
    const double step = RingStep (o);
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
        options.cut = s.cutSlope;
        options.fill = s.fillSlope;
        options.rounding = s.rounding;
    }
    return options;
}

bool EvaluateOutlineDesign (const edit::Document& doc, const EditTerrainInput& input, const OutlineDesignOptions& o,
                            TerrainDesignResult& result, uint32_t& unreached, std::string& error, uint32_t* clipped)
{
    const auto paved = edit::Pave (doc.paths, o.pavedStep, true, nullptr, kPavedTolerance);
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
    const ClipperPlanRing regionLocation (region);
    std::vector<std::pair<ClipperPlanRing, int>> gradedLocation;
    for (const auto& ring : graded)
        gradedLocation.emplace_back (ClipperPlanRing (ring), cp::Area (ring) > 0 ? 1 : -1);
    // The outline and the rings its faces lie on, inside the region.
    std::vector<std::vector<double>> lines;
    for (const auto& ring : graded)
        AddRing (ring, regionLocation, lines);
    // Each ring is clipped as an open curve: where the area's edge cuts it there is no face to
    // lie on, and closing it along that edge only fans needle triangles against the region ring.
    for (const double d : Distances (o)) {
        auto rings = cp::InflatePaths (graded, d, cp::JoinType::Round, cp::EndType::Polygon, 2.0, kPrecision, 0.05);
        for (auto& ring : rings)
            if (!ring.empty ())
                ring.push_back (ring.front ());
        cp::ClipperD clipper (kPrecision);
        clipper.AddOpenSubject (rings);
        clipper.AddClip (inner);
        cp::PathsD closed, open;
        if (!clipper.Execute (cp::ClipType::Intersection, cp::FillRule::NonZero, closed, open))
            continue;
        for (auto& piece : open) {
            if (piece.size () > 3 && piece.front () == piece.back ()) {
                piece.pop_back ();
                AddRing (piece, regionLocation, lines, RingStep (o));
            }
            else
                AddLine (piece, regionLocation, lines, RingStep (o));
        }
    }
    // The paved surface's own vertices inside it, so its profile and crossfall are kept.
    std::vector<double> spots;
    for (const auto& v : paved.vertices)
        if (InsideShape (gradedLocation, { v.x, v.y }) &&
            regionLocation.Locate ({ v.x, v.y }) == cp::PointInPolygonResult::IsInside)
            spots.insert (spots.end (), { v.x, v.y, 0.0 });
    std::vector<double> surface;
    for (const auto& v : paved.vertices)
        surface.insert (surface.end (), { v.x, v.y, v.z });
    const auto grades = SurfaceGrades (doc, paved.vertices);
    std::vector<double> regionRing;
    for (const auto& p : region)
        regionRing.insert (regionRing.end (), { p.x, p.y, 0.0 });
    OutlineTerrainRequest request;
    request.mesh = input.mesh;
    request.region = regionRing;
    request.surface = { surface, paved.triangles };
    request.lines = lines;
    request.spots = spots;
    request.surfaceGrades = grades;
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
    if (clipped)
        *clipped = built.clipped;
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
