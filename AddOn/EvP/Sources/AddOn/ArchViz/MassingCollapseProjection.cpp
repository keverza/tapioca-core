#include "ArchViz/MassingCollapseZone.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::massingcollapse {
namespace {
namespace cp = Clipper2Lib;
bool Fail (std::string& error, const char* text)
{
    error = text;
    return false;
}
} // namespace
bool Project (const Result& zone, const Mesh& terrain, overlaylayers::Layer& layer, std::string& error)
{
    error.clear ();
    if (terrain.vertices.empty () || terrain.vertices.size () % 3 || terrain.vertices.size () > 600000 ||
        terrain.triangles.empty () || terrain.triangles.size () % 3 || terrain.triangles.size () > 600000)
        return Fail (error, "Collapse projection awaits a bounded current topography mesh.");
    for (double value : terrain.vertices)
        if (!std::isfinite (value) || std::abs (value) > 1e9)
            return Fail (error, "Invalid topography coordinate.");
    for (auto index : terrain.triangles)
        if (index >= terrain.VertexCount ())
            return Fail (error, "Invalid topography triangle.");
    const double ox = terrain.vertices[0], oy = terrain.vertices[1];
    cp::PathsD paths;
    size_t zonePoints = 0;
    for (const auto& chain : zone.chains) {
        if (!chain.closed || chain.xy.size () % 2)
            return Fail (error, "Invalid collapse projection contour.");
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i) {
            if (!std::isfinite (chain.xy[i * 2]) || !std::isfinite (chain.xy[i * 2 + 1]))
                return Fail (error, "Invalid collapse projection contour.");
            path.emplace_back (chain.xy[i * 2] - ox, chain.xy[i * 2 + 1] - oy);
        }
        zonePoints += path.size ();
        paths.push_back (std::move (path));
    }
    if (zonePoints > 200000)
        return Fail (error, "Collapse projection contour budget exceeded.");
    const auto bounds = cp::GetBounds (paths);
    overlaylayers::Layer out;
    out.name = kProjectedLayer;
    out.views = overlaylayers::Views::ThreeD;
    out.occlusion = overlaylayers::Behind::Fade;
    overlaylayers::Mesh fill;
    fill.rgba = 0xAA446528;
    fill.styled = true;
    fill.style.behind = overlaylayers::Behind::Layer;
    const double phase = std::fmod (zone.hatchOriginSum - ox - oy, 0.7);
    size_t work = 0, topFaces = 0;
    for (size_t i = 0; i < terrain.triangles.size (); i += 3) {
        cp::PointD p[3];
        double z[3];
        for (size_t k = 0; k < 3; ++k) {
            const size_t at = size_t (terrain.triangles[i + k]) * 3;
            p[k] = { terrain.vertices[at] - ox, terrain.vertices[at + 1] - oy };
            z[k] = terrain.vertices[at + 2];
        }
        const double det = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
        if (det <= 1e-10)
            continue; // No vertical faces or downward-facing closed-mesh underside.
        ++topFaces;
        if (paths.empty ())
            continue;
        const auto triBounds = cp::GetBounds (cp::PathD { p[0], p[1], p[2] });
        if (triBounds.right < bounds.left || triBounds.left > bounds.right || triBounds.bottom < bounds.top ||
            triBounds.top > bounds.bottom)
            continue;
        work += zonePoints;
        if (work > 4000000)
            return Fail (error, "Collapse topography clipping work budget exceeded.");
        const double dx = ((z[1] - z[0]) * (p[2].y - p[0].y) - (z[2] - z[0]) * (p[1].y - p[0].y)) / det;
        const double dy = ((p[1].x - p[0].x) * (z[2] - z[0]) - (p[2].x - p[0].x) * (z[1] - z[0])) / det;
        const auto height = [&] (const cp::PointD& q) { return z[0] + dx * (q.x - p[0].x) + dy * (q.y - p[0].y); };
        const auto clipped = cp::Intersect (paths, { { p[0], p[1], p[2] } }, cp::FillRule::NonZero, 6);
        std::vector<SliceChain> chains;
        for (const auto& path : clipped) {
            SliceChain chain;
            chain.closed = true;
            for (const auto& q : path)
                chain.xy.insert (chain.xy.end (), { q.x, q.y });
            chains.push_back (std::move (chain));
        }
        std::vector<StorySliceFillVertex> triangles;
        BuildSliceFill (chains, 0, triangles);
        if (!clipped.empty () && triangles.empty ())
            return Fail (error, "Collapse topography patch could not be triangulated.");
        for (size_t k = 0; k + 2 < triangles.size (); k += 3) {
            cp::PointD triangle[3];
            for (size_t v = 0; v < 3; ++v) {
                triangle[v] = { triangles[k + v].x, triangles[k + v].y };
                fill.indices.push_back (uint32_t (fill.points.size () / 3));
                fill.points.insert (fill.points.end (),
                                    { triangle[v].x + ox, triangle[v].y + oy, height (triangle[v]) + 0.012 });
            }
            const double low = (std::min) ({ triangle[0].x + triangle[0].y, triangle[1].x + triangle[1].y,
                                             triangle[2].x + triangle[2].y });
            const double high = (std::max) ({ triangle[0].x + triangle[0].y, triangle[1].x + triangle[1].y,
                                              triangle[2].x + triangle[2].y });
            if ((high - low) / 0.7 > 20000)
                return Fail (error, "Collapse projected hatch span exceeds its budget.");
            for (double sum = phase + std::ceil ((low - phase) / 0.7) * 0.7; sum < high; sum += 0.7) {
                cp::PathD hits;
                for (size_t e = 0; e < 3; ++e) {
                    const auto& a = triangle[e];
                    const auto& b = triangle[(e + 1) % 3];
                    const double sa = a.x + a.y, sb = b.x + b.y;
                    if ((sa <= sum && sb > sum) || (sb <= sum && sa > sum)) {
                        const double t = (sum - sa) / (sb - sa);
                        hits.emplace_back (a.x + t * (b.x - a.x), a.y + t * (b.y - a.y));
                    }
                }
                if (hits.size () != 2 || std::hypot (hits[0].x - hits[1].x, hits[0].y - hits[1].y) < 1e-7)
                    continue;
                overlaylayers::Polyline line;
                line.rgba = 0xAA4465C0;
                line.widthPixels = 0.7f;
                line.behind = overlaylayers::Behind::Layer;
                for (const auto& q : hits)
                    line.points.insert (line.points.end (), { q.x + ox, q.y + oy, height (q) + 0.014 });
                out.polylines.push_back (std::move (line));
                if (out.polylines.size () > 100000)
                    return Fail (error, "Collapse projected hatch output budget exceeded.");
            }
            if (fill.indices.size () > 600000)
                return Fail (error, "Collapse projected fill output budget exceeded.");
        }
    }
    if (!topFaces)
        return Fail (error, "Topography has no upward-facing surface for collapse projection.");
    if (!fill.indices.empty ())
        out.meshes.push_back (std::move (fill));
    error = overlaylayers::Validate (out);
    if (!error.empty ())
        return false;
    layer = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingcollapse
