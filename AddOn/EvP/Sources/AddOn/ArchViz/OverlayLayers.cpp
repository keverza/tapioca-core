// ArchViz/OverlayLayers -- see the header.

#include "ArchViz/OverlayLayers.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {
namespace overlaylayers {

namespace {

constexpr size_t kMaxNameLength = 64;
// Two points closer than this are one point: a segment between them has no
// direction, and the stroke shader would draw it as a marker.
constexpr double kSamePoint = 1e-9;

std::vector<std::shared_ptr<const Layer>> g_layers; // MAIN THREAD
uint64_t g_generation = 0;

bool FiniteTriples (const std::vector<double>& points)
{
    if (points.size () % 3 != 0)
        return false;
    for (const double value : points)
        if (!std::isfinite (value))
            return false;
    return true;
}

bool Positive (float value, float most)
{
    return std::isfinite (value) && value > 0.0f && value <= most;
}

std::string Numbered (const char* what, size_t index, const std::string& problem)
{
    return std::string (what) + " " + std::to_string (index) + ": " + problem;
}

template <typename Visit> void EachPoint (const Layer& layer, Visit visit)
{
    for (const Polyline& polyline : layer.polylines)
        for (size_t i = 0; i + 2 < polyline.points.size (); i += 3)
            visit (polyline.points[i], polyline.points[i + 1], polyline.points[i + 2]);
    for (const PointSet& set : layer.points)
        for (size_t i = 0; i + 2 < set.points.size (); i += 3)
            visit (set.points[i], set.points[i + 1], set.points[i + 2]);
    for (const Mesh& mesh : layer.meshes)
        for (size_t i = 0; i + 2 < mesh.points.size (); i += 3)
            visit (mesh.points[i], mesh.points[i + 1], mesh.points[i + 2]);
}

void Split (double value, double origin, float& hi, float& lo)
{
    plancontent::Split (value - origin, hi, lo);
}

} // namespace

bool DrawnIn (Views views, Views view)
{
    return (uint32_t (views) & uint32_t (view)) != 0;
}

std::string Validate (const Layer& layer)
{
    if (layer.name.empty () || layer.name.size () > kMaxNameLength)
        return "a layer needs a name of 1 to 64 characters";
    for (size_t i = 0; i < layer.polylines.size (); ++i) {
        const Polyline& polyline = layer.polylines[i];
        if (!FiniteTriples (polyline.points))
            return Numbered ("polyline", i, "points are finite x, y, z triples");
        if (polyline.points.size () < 6)
            return Numbered ("polyline", i, "needs at least two points");
        if (!Positive (polyline.widthPixels, 64.0f))
            return Numbered ("polyline", i, "widthPixels must be above 0 and at most 64");
    }
    for (size_t i = 0; i < layer.points.size (); ++i) {
        const PointSet& set = layer.points[i];
        if (!FiniteTriples (set.points) || set.points.empty ())
            return Numbered ("point set", i, "points are finite x, y, z triples, at least one");
        if (!Positive (set.sizePixels, 256.0f))
            return Numbered ("point set", i, "sizePixels must be above 0 and at most 256");
        if (!Positive (set.sizeMetres, 1000.0f))
            return Numbered ("point set", i, "sizeMetres must be above 0 and at most 1000");
    }
    for (size_t i = 0; i < layer.meshes.size (); ++i) {
        const Mesh& mesh = layer.meshes[i];
        if (!FiniteTriples (mesh.points) || mesh.points.size () < 9)
            return Numbered ("mesh", i, "points are finite x, y, z triples, at least three");
        if (mesh.indices.empty () || mesh.indices.size () % 3 != 0)
            return Numbered ("mesh", i, "indices come in threes, one triple per triangle");
        const size_t vertices = mesh.points.size () / 3;
        for (const uint32_t index : mesh.indices)
            if (index >= vertices)
                return Numbered ("mesh", i,
                                 "index " + std::to_string (index) + " is past its " + std::to_string (vertices) +
                                     " vertices");
        if (!mesh.vertexRgba.empty () && mesh.vertexRgba.size () != vertices)
            return Numbered ("mesh", i, "vertexColors has one colour per vertex or none");
    }
    return std::string ();
}

Summary Summarise (const Layer& layer)
{
    Summary summary;
    summary.name = layer.name;
    summary.views = layer.views;
    summary.occluded = layer.occluded;
    summary.polylines = uint32_t (layer.polylines.size ());
    for (const Polyline& polyline : layer.polylines)
        summary.lineVertices += uint32_t (polyline.points.size () / 3);
    for (const PointSet& set : layer.points)
        summary.points += uint32_t (set.points.size () / 3);
    summary.meshes = uint32_t (layer.meshes.size ());
    for (const Mesh& mesh : layer.meshes)
        summary.triangles += uint32_t (mesh.indices.size () / 3);
    return summary;
}

uint64_t Set (Layer layer)
{
    std::shared_ptr<const Layer> shared = std::make_shared<const Layer> (std::move (layer));
    for (std::shared_ptr<const Layer>& existing : g_layers) {
        if (existing->name == shared->name) {
            existing = std::move (shared);
            return ++g_generation;
        }
    }
    g_layers.push_back (std::move (shared));
    return ++g_generation;
}

bool Clear (const std::string& name)
{
    const auto end =
        std::remove_if (g_layers.begin (), g_layers.end (),
                        [&name] (const std::shared_ptr<const Layer>& layer) { return layer->name == name; });
    if (end == g_layers.end ())
        return false;
    g_layers.erase (end, g_layers.end ());
    ++g_generation;
    return true;
}

void ClearAll ()
{
    if (g_layers.empty ())
        return;
    g_layers.clear ();
    ++g_generation;
}

std::vector<std::shared_ptr<const Layer>> Layers ()
{
    return g_layers;
}

uint64_t Generation ()
{
    return g_generation;
}

uint32_t ToUnorm (uint32_t rgba)
{
    const uint32_t red = (rgba >> 24) & 0xFFu;
    const uint32_t green = (rgba >> 16) & 0xFFu;
    const uint32_t blue = (rgba >> 8) & 0xFFu;
    const uint32_t alpha = rgba & 0xFFu;
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

Prepared2D Prepare2D (const std::vector<std::shared_ptr<const Layer>>& layers)
{
    Prepared2D out;
    double sumX = 0.0, sumY = 0.0;
    size_t count = 0;
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::TwoD))
            continue;
        EachPoint (*layer, [&] (double x, double y, double) {
            sumX += x;
            sumY += y;
            ++count;
        });
    }
    if (count == 0)
        return out;
    out.originX = sumX / double (count);
    out.originY = sumY / double (count);

    auto stroke = [&out] (double ax, double ay, double bx, double by, uint32_t rgba, float width) {
        StrokeInstance instance;
        Split (ax, out.originX, instance.segment.x0, instance.segment.x0Lo);
        Split (ay, out.originY, instance.segment.y0, instance.segment.y0Lo);
        Split (bx, out.originX, instance.segment.x1, instance.segment.x1Lo);
        Split (by, out.originY, instance.segment.y1, instance.segment.y1Lo);
        instance.rgba = ToUnorm (rgba);
        instance.widthPixels = width;
        out.strokes.push_back (instance);
    };

    // ⚠️ FILLS FIRST, STROKES OVER THEM: an outline drawn under its own fill is an
    // outline nobody sees.
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::TwoD))
            continue;
        for (const Mesh& mesh : layer->meshes) {
            for (const uint32_t index : mesh.indices) {
                FillVertex vertex;
                Split (mesh.points[size_t (index) * 3], out.originX, vertex.x, vertex.xLo);
                Split (mesh.points[size_t (index) * 3 + 1], out.originY, vertex.y, vertex.yLo);
                vertex.rgba = ToUnorm (mesh.vertexRgba.empty () ? mesh.rgba : mesh.vertexRgba[index]);
                out.fills.push_back (vertex);
            }
        }
    }
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::TwoD))
            continue;
        for (const Polyline& polyline : layer->polylines) {
            const size_t points = polyline.points.size () / 3;
            const size_t segments = polyline.closed ? points : points - 1;
            for (size_t i = 0; i < segments; ++i) {
                const size_t a = i, b = (i + 1) % points;
                const double ax = polyline.points[a * 3], ay = polyline.points[a * 3 + 1];
                const double bx = polyline.points[b * 3], by = polyline.points[b * 3 + 1];
                if (std::fabs (bx - ax) < kSamePoint && std::fabs (by - ay) < kSamePoint)
                    continue;
                stroke (ax, ay, bx, by, polyline.rgba, polyline.widthPixels);
            }
        }
        // A marker is a stroke of no length: the shader squares it, `sizePixels` wide.
        for (const PointSet& set : layer->points)
            for (size_t i = 0; i + 2 < set.points.size (); i += 3)
                stroke (set.points[i], set.points[i + 1], set.points[i], set.points[i + 1], set.rgba, set.sizePixels);
    }
    return out;
}

Prepared3D Prepare3D (const std::vector<std::shared_ptr<const Layer>>& layers)
{
    Prepared3D out;
    auto vertex = [] (double x, double y, double z, uint32_t rgba) {
        ColourVertex v;
        v.x = float (x);
        v.y = float (y);
        v.z = float (z);
        v.rgba = rgba;
        return v;
    };
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::ThreeD))
            continue;
        std::vector<ColourVertex>& lines = layer->occluded ? out.occludedLines : out.overLines;
        std::vector<ColourVertex>& fills = layer->occluded ? out.occludedFills : out.overFills;
        for (const Polyline& polyline : layer->polylines) {
            const uint32_t rgba = ToUnorm (polyline.rgba);
            const size_t points = polyline.points.size () / 3;
            const size_t segments = polyline.closed ? points : points - 1;
            for (size_t i = 0; i < segments; ++i) {
                const size_t a = i * 3, b = ((i + 1) % points) * 3;
                lines.push_back (vertex (polyline.points[a], polyline.points[a + 1], polyline.points[a + 2], rgba));
                lines.push_back (vertex (polyline.points[b], polyline.points[b + 1], polyline.points[b + 2], rgba));
            }
        }
        for (const PointSet& set : layer->points) {
            const uint32_t rgba = ToUnorm (set.rgba);
            const double half = double (set.sizeMetres) * 0.5;
            for (size_t i = 0; i + 2 < set.points.size (); i += 3) {
                const double x = set.points[i], y = set.points[i + 1], z = set.points[i + 2];
                lines.push_back (vertex (x - half, y, z, rgba));
                lines.push_back (vertex (x + half, y, z, rgba));
                lines.push_back (vertex (x, y - half, z, rgba));
                lines.push_back (vertex (x, y + half, z, rgba));
                lines.push_back (vertex (x, y, z - half, rgba));
                lines.push_back (vertex (x, y, z + half, rgba));
            }
        }
        for (const Mesh& mesh : layer->meshes) {
            for (const uint32_t index : mesh.indices) {
                const size_t at = size_t (index) * 3;
                const uint32_t rgba = ToUnorm (mesh.vertexRgba.empty () ? mesh.rgba : mesh.vertexRgba[index]);
                fills.push_back (vertex (mesh.points[at], mesh.points[at + 1], mesh.points[at + 2], rgba));
            }
        }
    }
    return out;
}

} // namespace overlaylayers
} // namespace archviz
} // namespace geomsrv
