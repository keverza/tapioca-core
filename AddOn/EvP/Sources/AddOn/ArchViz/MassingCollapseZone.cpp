#include "ArchViz/MassingCollapseZone.hpp"
#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>

namespace geomsrv::archviz::massingcollapse {
namespace {
namespace cp = Clipper2Lib;
constexpr double kPi = 3.14159265358979323846;
constexpr double kArcTolerance = 0.01; // conservative polygonal circle error, metres
struct Facet {
    cp::PathD xy;
    double dx = 0, dy = 0, z = 0;
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    double Height (const cp::PointD& p) const
    {
        return dx * p.x + dy * p.y + z;
    }
};
bool Fail (std::string& error, const char* text)
{
    error = text;
    return false;
}
double Cross (const cp::PointD& a, const cp::PointD& b, const cp::PointD& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
cp::PathD Hull (cp::PathD points)
{
    std::sort (points.begin (), points.end (),
               [] (const auto& a, const auto& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    points.erase (std::unique (points.begin (), points.end (),
                               [] (const auto& a, const auto& b) { return a.x == b.x && a.y == b.y; }),
                  points.end ());
    if (points.size () < 3)
        return {};
    cp::PathD hull;
    for (const auto& p : points) {
        while (hull.size () >= 2 && Cross (hull[hull.size () - 2], hull.back (), p) <= 0)
            hull.pop_back ();
        hull.push_back (p);
    }
    const size_t lower = hull.size ();
    for (size_t i = points.size () - 1; i-- > 0;) {
        while (hull.size () > lower && Cross (hull[hull.size () - 2], hull.back (), points[i]) <= 0)
            hull.pop_back ();
        hull.push_back (points[i]);
    }
    hull.pop_back ();
    return hull;
}
cp::PathD Half (const cp::PathD& path, const Facet& a, const Facet& b, double sign)
{
    cp::PathD out;
    for (size_t i = 0; i < path.size (); ++i) {
        const auto& p = path[i];
        const auto& q = path[(i + 1) % path.size ()];
        const double hp = sign * (a.Height (p) - b.Height (p));
        const double hq = sign * (a.Height (q) - b.Height (q));
        if (hp >= 0)
            out.push_back (p);
        if ((hp < 0) != (hq < 0)) {
            const double t = hp / (hp - hq);
            out.emplace_back (p.x + t * (q.x - p.x), p.y + t * (q.y - p.y));
        }
    }
    return out;
}
bool Sweep (const cp::PathD& patch, const Facet& a, const Facet& b, cp::PathsD& regions, size_t& points,
            std::string& error)
{
    if (patch.size () < 3 || std::abs (cp::Area (patch)) < 1e-10)
        return true;
    cp::PathD cloud;
    for (const auto& p : patch) {
        const double radius = std::abs (a.Height (p) - b.Height (p)) * kHeightFactor;
        if (!std::isfinite (radius) || radius > 10000)
            return Fail (error, "Collapse-zone local height is out of range.");
        if (radius < 1e-8) {
            cloud.push_back (p);
            continue;
        }
        const int steps = (std::max) (16, int (std::ceil (kPi / std::acos (radius / (radius + kArcTolerance)))));
        if (steps > 4096)
            return Fail (error, "Collapse-zone round offset exceeds its precision budget.");
        const double conservativeRadius = radius / std::cos (kPi / steps);
        points += size_t (steps);
        if (points > 2000000)
            return Fail (error, "Collapse-zone round offset exceeds its geometry budget.");
        for (int j = 0; j < steps; ++j) {
            const double angle = 2 * kPi * j / steps;
            cloud.emplace_back (p.x + conservativeRadius * std::cos (angle),
                                p.y + conservativeRadius * std::sin (angle));
        }
    }
    // Radius is affine within each intersected/split surface patch. Its complete
    // variable-radius sweep is the convex hull of its vertex discs, NOT a single
    // maximum-radius buffer or a sampled/flattened footprint.
    auto hull = Hull (std::move (cloud));
    if (!hull.empty ())
        regions.push_back (std::move (hull));
    return true;
}
bool Facets (const Mesh& body, double ox, double oy, std::vector<Facet>& facets, std::string& error)
{
    if (body.vertices.empty () || body.triangles.empty () || body.vertices.size () % 3 || body.triangles.size () % 3 ||
        body.TriangleCount () > 50000)
        return Fail (error, "Collapse zone requires a complete bounded 3D slab body.");
    for (double value : body.vertices)
        if (!std::isfinite (value) || std::abs (value) > 1e9)
            return Fail (error, "Invalid collapse-zone body coordinate.");
    using Key = std::array<int64_t, 3>;
    std::map<std::pair<Key, Key>, unsigned> edges;
    for (size_t i = 0; i < body.triangles.size (); i += 3) {
        cp::PointD p[3];
        double z[3];
        Key keys[3];
        for (int k = 0; k < 3; ++k) {
            const size_t at = size_t (body.triangles[i + k]) * 3;
            if (at + 2 >= body.vertices.size ())
                return Fail (error, "Invalid collapse-zone body index.");
            p[k] = { body.vertices[at] - ox, body.vertices[at + 1] - oy };
            z[k] = body.vertices[at + 2];
            keys[k] = { int64_t (std::llround (p[k].x * 1e6)), int64_t (std::llround (p[k].y * 1e6)),
                        int64_t (std::llround (z[k] * 1e6)) };
        }
        for (int k = 0; k < 3; ++k) {
            const auto& a = keys[k];
            const auto& b = keys[(k + 1) % 3];
            if (a != b)
                ++edges[a < b ? std::make_pair (a, b) : std::make_pair (b, a)];
        }
        const double det = Cross (p[0], p[1], p[2]);
        if (std::abs (det) < 1e-10)
            continue; // vertical faces have no footprint area; their adjoining facets retain the edge
        Facet f;
        f.xy = { p[0], p[1], p[2] };
        if (det < 0)
            std::reverse (f.xy.begin (), f.xy.end ());
        f.dx = ((z[1] - z[0]) * (p[2].y - p[0].y) - (z[2] - z[0]) * (p[1].y - p[0].y)) / det;
        f.dy = ((p[1].x - p[0].x) * (z[2] - z[0]) - (p[2].x - p[0].x) * (z[1] - z[0])) / det;
        f.z = z[0] - f.dx * p[0].x - f.dy * p[0].y;
        f.minX = (std::min) ({ p[0].x, p[1].x, p[2].x });
        f.maxX = (std::max) ({ p[0].x, p[1].x, p[2].x });
        f.minY = (std::min) ({ p[0].y, p[1].y, p[2].y });
        f.maxY = (std::max) ({ p[0].y, p[1].y, p[2].y });
        facets.push_back (std::move (f));
    }
    for (const auto& edge : edges)
        if (edge.second != 2)
            return Fail (error, "Collapse zone awaits a closed manifold body; no footprint substitute used.");
    return true;
}
} // namespace

bool Build (const std::vector<massingslices::Input>& inputs, double drawingZ, Result& result, std::string& error)
{
    error.clear ();
    if (!std::isfinite (drawingZ) || inputs.size () > 128)
        return Fail (error, "Invalid collapse-zone drawing plane or source budget.");
    Result out;
    out.layer.name = kLayer;
    out.layer.views = overlaylayers::Views::Both;
    out.layer.occlusion = overlaylayers::Behind::Show;
    if (inputs.empty ()) {
        result = std::move (out);
        return true;
    }
    double ox = 0, oy = 0;
    std::map<std::string, std::vector<Facet>> buildings;
    size_t totalFacets = 0;
    for (const auto& input : inputs) {
        if (!input.body || input.body->vertices.empty ())
            return Fail (error, "Collapse zone awaiting current 3D slab bodies (including SEO/sloped surfaces).");
        if (buildings.empty ()) {
            ox = input.body->vertices[0];
            oy = input.body->vertices[1];
        }
        const auto* id = metadata::FindProperty (input.metadata, "massing.buildingId");
        const auto building = id && !id->value.s.empty () ? "building:" + id->value.s : "slab:" + input.slab.guid;
        auto& facets = buildings[building];
        const size_t before = facets.size ();
        if (!Facets (*input.body, ox, oy, facets, error))
            return false;
        totalFacets += facets.size () - before;
        if (totalFacets > 10000)
            return Fail (error, "Collapse-zone surface partition exceeds its facet budget.");
    }
    cp::PathsD regions;
    size_t visits = 0, points = 0;
    const auto began = std::chrono::steady_clock::now ();
    for (auto& building : buildings) {
        auto& facets = building.second;
        std::sort (facets.begin (), facets.end (), [] (const auto& a, const auto& b) { return a.minX < b.minX; });
        for (size_t i = 0; i < facets.size (); ++i)
            for (size_t j = i + 1; j < facets.size () && facets[j].minX < facets[i].maxX; ++j) {
                if (++visits > 200000 ||
                    (visits % 256 == 0 && std::chrono::steady_clock::now () - began > std::chrono::seconds (2)))
                    return Fail (error, "Collapse-zone surface intersections exceed their work/time budget.");
                const auto& a = facets[i];
                const auto& b = facets[j];
                if (b.minY >= a.maxY || b.maxY <= a.minY)
                    continue;
                for (const auto& overlap :
                     cp::Intersect (cp::PathsD { a.xy }, cp::PathsD { b.xy }, cp::FillRule::NonZero, 6))
                    for (double sign : { -1.0, 1.0 })
                        if (!Sweep (Half (overlap, a, b, sign), a, b, regions, points, error))
                            return false;
            }
    }
    const auto unioned = cp::Union (regions, cp::FillRule::NonZero, 6);
    size_t unionPoints = 0;
    for (const auto& path : unioned)
        unionPoints += path.size ();
    if (unionPoints > 200000)
        return Fail (error, "Collapse-zone union exceeds its contour budget.");
    out.area = std::abs (cp::Area (unioned));
    std::vector<SliceChain> local;
    for (const auto& path : unioned) {
        SliceChain chain;
        chain.closed = true;
        for (const auto& p : path)
            chain.xy.insert (chain.xy.end (), { p.x, p.y });
        local.push_back (chain);
        for (size_t i = 0; i < chain.Count (); ++i) {
            chain.xy[i * 2] += ox;
            chain.xy[i * 2 + 1] += oy;
        }
        out.chains.push_back (std::move (chain));
    }
    std::vector<StorySliceFillVertex> triangles;
    BuildSliceFill (local, 0, triangles);
    if (triangles.size () > 600000)
        return Fail (error, "Collapse-zone fill exceeds its triangle-vertex budget.");
    if (!unioned.empty () && triangles.empty ())
        return Fail (error, "Collapse-zone union could not be triangulated.");
    overlaylayers::Mesh fill;
    fill.rgba = 0xAA446528;
    fill.styled = true;
    fill.style.behind = overlaylayers::Behind::Show;
    fill.hoverTitle = "Building collapse zone (0.3333 x local height)";
    for (const auto& v : triangles) {
        fill.indices.push_back (uint32_t (fill.points.size () / 3));
        fill.points.insert (fill.points.end (), { ox + v.x, oy + v.y, drawingZ + 0.01 });
    }
    if (!fill.indices.empty ())
        out.layer.meshes.push_back (std::move (fill));
    // Model-spaced 45-degree hatches, clipped against the UNION including holes.
    const auto bounds = cp::GetBounds (unioned);
    const double start = std::floor ((bounds.left + bounds.top) / 0.7) * 0.7;
    const double end = bounds.right + bounds.bottom;
    if (!unioned.empty () && (end - start) / 0.7 > 20000)
        return Fail (error, "Collapse-zone hatch exceeds its line budget.");
    cp::PathsD hatch;
    if (!unioned.empty ())
        for (double sum = start; sum <= end; sum += 0.7)
            hatch.push_back (
                { { bounds.left - 1, sum - bounds.left + 1 }, { bounds.right + 1, sum - bounds.right - 1 } });
    cp::ClipperD clipper (6);
    clipper.AddOpenSubject (hatch);
    clipper.AddClip (unioned);
    cp::PathsD closed, clippedHatch;
    clipper.Execute (cp::ClipType::Intersection, cp::FillRule::NonZero, closed, clippedHatch);
    if (clippedHatch.size () > 100000)
        return Fail (error, "Collapse-zone clipped hatch exceeds its output budget.");
    for (const auto& path : clippedHatch) {
        overlaylayers::Polyline line;
        line.rgba = 0xAA4465C0;
        line.behind = overlaylayers::Behind::Show;
        for (const auto& p : path)
            line.points.insert (line.points.end (), { p.x + ox, p.y + oy, drawingZ + 0.012 });
        if (line.points.size () >= 6)
            out.layer.polylines.push_back (std::move (line));
    }
    error = overlaylayers::Validate (out.layer);
    if (!error.empty ())
        return false;
    result = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingcollapse
