#include "ArchViz/RoofSlope.hpp"
#include "ArchViz/HudShell.hpp"
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

// The roof's command-scoped overlay layer: shaded faces, breaklines, fall arrows with
// model-space slope labels and drain markers, plus the shared HUD panel look.
namespace geomsrv::archviz::roofslope {
namespace {
double Cross (Vec a, Vec b, Vec c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
std::string Number (double value, int decimals)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision (decimals) << value;
    return out.str ();
}
} // namespace

overlaylayers::Layer Layer (const Data& d)
{
    namespace ol = overlaylayers;
    ol::Layer layer;
    layer.name = kLayer;
    layer.lifetime = ol::LayerLifetime::Command;
    layer.views = ol::Views::Both;
    layer.occlusion = ol::Behind::Fade;
    ol::Mesh mesh;
    mesh.rgba = 0x839987FF;
    mesh.styled = true;
    mesh.style.opacity = 0.24f;
    auto xyz = [&] (int n) {
        Vec p = World (d, d.nodes[n].xy);
        return std::array<double, 3> { p.x, p.y, d.nodes[n].z };
    };
    auto line = [&] (std::array<double, 3> a, std::array<double, 3> b, uint32_t color, float width) {
        ol::Polyline p;
        p.points = { a[0], a[1], a[2], b[0], b[1], b[2] };
        p.rgba = color, p.widthPixels = width;
        layer.polylines.push_back (std::move (p));
    };
    struct Edge {
        std::array<double, 3> a, b;
        std::array<int, 3> face;
        int count = 0;
        bool breakline = false;
    };
    std::map<std::array<int64_t, 4>, Edge> edges;
    std::map<std::array<int, 3>, const Triangle*> labels;
    for (const auto& t : d.triangles) {
        const uint32_t first = uint32_t (mesh.points.size () / 3);
        std::array<std::array<double, 3>, 3> vertices;
        for (int k = 0; k < 3; ++k) {
            Vec p = World (d, t.vertices[k].xy);
            vertices[k] = { p.x, p.y, t.vertices[k].z };
            mesh.points.insert (mesh.points.end (), vertices[k].begin (), vertices[k].end ());
        }
        mesh.indices.insert (mesh.indices.end (), { first, first + 1, first + 2 });
        for (int j = 0; j < 3; ++j) {
            auto a = vertices[j], b = vertices[(j + 1) % 3];
            if (a > b)
                std::swap (a, b);
            std::array<int64_t, 4> key { std::llround (a[0] * 1e6), std::llround (a[1] * 1e6),
                                         std::llround (b[0] * 1e6), std::llround (b[1] * 1e6) };
            auto& e = edges[key];
            if (e.count == 0)
                e.a = a, e.b = b, e.face = t.nodes;
            e.breakline = e.breakline || e.face != t.nodes;
            ++e.count;
        }
        if (!labels.contains (t.nodes) || labels[t.nodes]->area < t.area)
            labels[t.nodes] = &t;
    }
    for (const auto& [face, triangle] : labels) {
        const auto& t = *triangle;
        const double slope = std::hypot (t.gx, t.gy);
        Vec center {};
        double z = 0;
        for (const auto& n : t.vertices)
            center.x += n.xy.x / 3, center.y += n.xy.y / 3, z += n.z / 3;
        Vec wc = World (d, center);
        double length = (std::min) (1.5, std::sqrt (t.area) * 0.3);
        const Vec down { -t.gx / slope, -t.gy / slope };
        const double winding = Cross (t.vertices[0].xy, t.vertices[1].xy, t.vertices[2].xy) > 0 ? 1 : -1;
        for (int j = 0; j < 3; ++j) {
            const Vec a = t.vertices[j].xy, b = t.vertices[(j + 1) % 3].xy;
            const double derivative = winding * ((b.x - a.x) * down.y - (b.y - a.y) * down.x);
            if (derivative < -1e-10)
                length = (std::min) (length, 0.75 * winding * Cross (a, b, center) / -derivative);
        }
        Vec end = World (d, { center.x - t.gx / slope * length, center.y - t.gy / slope * length });
        line ({ wc.x, wc.y, z + 0.005 }, { end.x, end.y, z - slope * length + 0.005 }, 0x5A7F62FF, 1.5f);
        layer.polylines.back ().endArrow = ol::Terminator::Arrow;
        ol::Text text;
        text.text = Number (slope * 100, 2) + "%";
        text.at[0] = wc.x, text.at[1] = wc.y, text.at[2] = z + 0.01;
        text.planar = true, text.sizeMetres = (std::max) (0.30, d.textHeight);
        text.rgba = 0x37523DFF, text.haloRgba = 0, text.haloPixels = 0;
        text.font = "tapioca:roof-inter";
        const double cs = std::cos (d.bearing), sn = std::sin (d.bearing);
        const double gx = t.gx * cs - t.gy * sn, gy = t.gx * sn + t.gy * cs;
        // Model-space text runs along the actual fall arrow, kept upright in plan.
        const double sign = gx > 0 || (std::abs (gx) < 1e-9 && gy > 0) ? 1 : -1;
        text.direction[0] = sign * gx / slope, text.direction[1] = sign * gy / slope;
        text.direction[2] = sign * slope;
        text.at[0] -= text.direction[1] * text.sizeMetres * 0.7;
        text.at[1] += text.direction[0] * text.sizeMetres * 0.7;
        const double normal = std::sqrt (1 + gx * gx + gy * gy);
        text.normal[0] = -gx / normal, text.normal[1] = -gy / normal, text.normal[2] = 1 / normal;
        layer.texts.push_back (text);
    }
    for (const auto& [key, edge] : edges)
        if (edge.count == 1 || edge.breakline)
            line (edge.a, edge.b, 0x37523DFF, 1.2f);
    ol::PointSet points;
    points.rgba = 0x5A7F62FF;
    points.sizeMetres = 0.15f;
    for (const auto& c : d.cells) {
        if (!c.drainInside)
            continue;
        auto p = xyz (c.drain);
        points.points.insert (points.points.end (), p.begin (), p.end ());
    }
    if (!points.points.empty ())
        layer.points.push_back (std::move (points));
    layer.meshes.push_back (std::move (mesh));
    layer.panels.push_back (PanelStyle ());
    return layer;
}
overlaylayers::Panel PanelStyle ()
{
    // OVERLAY-INVARIANTS.md: change editor content, not the shared HUD's look.
    auto panel = hudshell::PlainLook ();
    panel.title = "Roof slopes";
    panel.widthPixels = 360;
    return panel;
}
overlaylayers::Panel EditorStyle ()
{
    return hudshell::PlainLook ();
}
} // namespace geomsrv::archviz::roofslope
