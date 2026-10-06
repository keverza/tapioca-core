#include "ArchViz/TerrainProjection.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::terrainprojection {
namespace {
namespace cp = Clipper2Lib;
bool Fail (std::string& error, const char* text)
{
    error = text;
    return false;
}
} // namespace
bool HatchPlan (const std::vector<SliceChain>& chains, double originSum, double z, const Style& style,
                overlaylayers::Layer& layer, std::string& error)
{
    error.clear ();
    if (!std::isfinite (originSum) || !std::isfinite (z) || !std::isfinite (style.spacing) || style.spacing <= 0 ||
        !std::isfinite (style.hatchWidthPixels) || style.hatchWidthPixels <= 0)
        return Fail (error, "Invalid plan hatch style or elevation.");
    overlaylayers::Layer out;
    out.name = style.name;
    out.graphicsCategory = style.category;
    out.views = overlaylayers::Views::TwoD;
    out.occlusion = overlaylayers::Behind::Show;
    double ox = 0, oy = 0;
    if (!chains.empty () && chains[0].Count () > 0) {
        ox = chains[0].xy[0];
        oy = chains[0].xy[1];
    }
    cp::PathsD paths;
    size_t points = 0;
    for (const auto& chain : chains) {
        if (!chain.closed || chain.xy.size () % 2 || chain.Count () < 3)
            return Fail (error, "Invalid plan hatch contour.");
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i) {
            if (!std::isfinite (chain.xy[i * 2]) || !std::isfinite (chain.xy[i * 2 + 1]) ||
                std::abs (chain.xy[i * 2]) > 1e9 || std::abs (chain.xy[i * 2 + 1]) > 1e9)
                return Fail (error, "Invalid plan hatch coordinate.");
            path.emplace_back (chain.xy[i * 2] - ox, chain.xy[i * 2 + 1] - oy);
        }
        points += path.size ();
        if (points > 200000)
            return Fail (error, "Plan hatch contour budget exceeded.");
        paths.push_back (std::move (path));
    }
    paths = cp::Union (paths, cp::FillRule::NonZero, 6);
    const auto bounds = cp::GetBounds (paths);
    const double phase = std::fmod (originSum - ox - oy, style.spacing);
    const double start = phase + std::ceil ((bounds.left + bounds.top - phase) / style.spacing) * style.spacing;
    const double end = bounds.right + bounds.bottom;
    if (!paths.empty () && (end - start) / style.spacing > 20000)
        return Fail (error, "Plan hatch span budget exceeded.");
    cp::PathsD hatches;
    if (!paths.empty ())
        for (double sum = start; sum <= end; sum += style.spacing)
            hatches.push_back (
                { { bounds.left - 1, sum - bounds.left + 1 }, { bounds.right + 1, sum - bounds.right - 1 } });
    cp::ClipperD clipper (6);
    clipper.AddOpenSubject (hatches);
    clipper.AddClip (paths);
    cp::PathsD closed, clipped;
    clipper.Execute (cp::ClipType::Intersection, cp::FillRule::NonZero, closed, clipped);
    if (clipped.size () > 100000)
        return Fail (error, "Plan hatch output budget exceeded.");
    for (const auto& path : clipped) {
        overlaylayers::Polyline line;
        line.rgba = style.hatchRgba;
        line.widthPixels = style.hatchWidthPixels;
        line.behind = overlaylayers::Behind::Show;
        for (const auto& p : path)
            line.points.insert (line.points.end (), { p.x + ox, p.y + oy, z + 0.014 });
        if (line.points.size () >= 6)
            out.polylines.push_back (std::move (line));
    }
    error = overlaylayers::Validate (out);
    if (!error.empty ())
        return false;
    layer = std::move (out);
    return true;
}

bool Project (const std::vector<SliceChain>& chains, double originSum, const Mesh& terrain, const Style& style,
              overlaylayers::Layer& layer, std::string& error)
{
    error.clear ();
    if (!std::isfinite (originSum) || !std::isfinite (style.spacing) || style.spacing <= 0 ||
        !std::isfinite (style.hatchWidthPixels) || style.hatchWidthPixels <= 0)
        return Fail (error, "Invalid terrain hatch style.");
    if (terrain.vertices.empty () || terrain.vertices.size () % 3 || terrain.vertices.size () > 600000 ||
        terrain.triangles.empty () || terrain.triangles.size () % 3 || terrain.triangles.size () > 600000)
        return Fail (error, "Terrain projection awaits a bounded current topography mesh.");
    for (double value : terrain.vertices)
        if (!std::isfinite (value) || std::abs (value) > 1e9)
            return Fail (error, "Invalid topography coordinate.");
    for (auto index : terrain.triangles)
        if (index >= terrain.VertexCount ())
            return Fail (error, "Invalid topography triangle.");
    const double ox = terrain.vertices[0], oy = terrain.vertices[1];
    cp::PathsD paths;
    size_t zonePoints = 0;
    for (const auto& chain : chains) {
        if (!chain.closed || chain.xy.size () % 2 || chain.Count () < 3)
            return Fail (error, "Invalid collapse projection contour.");
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i) {
            if (!std::isfinite (chain.xy[i * 2]) || !std::isfinite (chain.xy[i * 2 + 1]) ||
                std::abs (chain.xy[i * 2]) > 1e9 || std::abs (chain.xy[i * 2 + 1]) > 1e9)
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
    out.name = style.name;
    out.graphicsCategory = style.category;
    out.views = overlaylayers::Views::ThreeD;
    out.occlusion = overlaylayers::Behind::Fade;
    overlaylayers::Mesh fill;
    fill.rgba = style.fillRgba;
    fill.styled = true;
    fill.style.behind = overlaylayers::Behind::Layer;
    const double phase = std::fmod (originSum - ox - oy, style.spacing);
    size_t work = 0, topFaces = 0, fillVertices = 0;
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
        std::vector<SliceChain> patches;
        for (const auto& path : clipped) {
            SliceChain chain;
            chain.closed = true;
            for (const auto& q : path)
                chain.xy.insert (chain.xy.end (), { q.x, q.y });
            patches.push_back (std::move (chain));
        }
        std::vector<StorySliceFillVertex> triangles;
        BuildSliceFill (patches, 0, triangles);
        if (!clipped.empty () && triangles.empty ())
            return Fail (error, "Collapse topography patch could not be triangulated.");
        for (size_t k = 0; k + 2 < triangles.size (); k += 3) {
            cp::PointD triangle[3];
            for (size_t v = 0; v < 3; ++v) {
                triangle[v] = { triangles[k + v].x, triangles[k + v].y };
                ++fillVertices;
                if ((style.fillRgba & 255u) != 0) {
                    fill.indices.push_back (uint32_t (fill.points.size () / 3));
                    fill.points.insert (fill.points.end (),
                                        { triangle[v].x + ox, triangle[v].y + oy, height (triangle[v]) + 0.012 });
                }
            }
            const double low = (std::min) ({ triangle[0].x + triangle[0].y, triangle[1].x + triangle[1].y,
                                             triangle[2].x + triangle[2].y });
            const double high = (std::max) ({ triangle[0].x + triangle[0].y, triangle[1].x + triangle[1].y,
                                              triangle[2].x + triangle[2].y });
            if ((high - low) / style.spacing > 20000)
                return Fail (error, "Collapse projected hatch span exceeds its budget.");
            for (double sum = phase + std::ceil ((low - phase) / style.spacing) * style.spacing; sum < high;
                 sum += style.spacing) {
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
                line.rgba = style.hatchRgba;
                line.widthPixels = style.hatchWidthPixels;
                line.behind = overlaylayers::Behind::Layer;
                for (const auto& q : hits)
                    line.points.insert (line.points.end (), { q.x + ox, q.y + oy, height (q) + 0.014 });
                out.polylines.push_back (std::move (line));
                if (out.polylines.size () > 100000)
                    return Fail (error, "Collapse projected hatch output budget exceeded.");
            }
            if (fillVertices > 600000)
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
} // namespace geomsrv::archviz::terrainprojection
