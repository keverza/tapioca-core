#include "ArchViz/EditLayers.hpp"
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::editlayers {

namespace layers = overlaylayers;
using edit::Vec3;

std::string FormatValue (edit::Quantity quantity, double value)
{
    char text[48];
    switch (quantity) {
        case edit::Quantity::Slope:
            std::snprintf (text, sizeof text, "%.1f %%", value);
            break;
        case edit::Quantity::Angle:
            std::snprintf (text, sizeof text, "%.1f\xC2\xB0", value);
            break;
        case edit::Quantity::Z:
        case edit::Quantity::Elevation:
            std::snprintf (text, sizeof text, "%+.2f m", value);
            break;
        default:
            std::snprintf (text, sizeof text, "%.2f m", value);
            break;
    }
    return text;
}

std::vector<uint32_t> TriangulateXY (const std::vector<Vec3>& polygon)
{
    std::vector<uint32_t> out;
    std::vector<uint32_t> ring;
    for (uint32_t i = 0; i < polygon.size (); ++i)
        ring.push_back (i);
    const auto cross = [&] (uint32_t a, uint32_t b, uint32_t c) {
        const Vec3 p = polygon[a], q = polygon[b], r = polygon[c];
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    const auto inside = [&] (uint32_t a, uint32_t b, uint32_t c, uint32_t p) {
        return cross (a, b, p) >= 0.0 && cross (b, c, p) >= 0.0 && cross (c, a, p) >= 0.0;
    };
    size_t guard = ring.size () * ring.size () + 8;
    while (ring.size () > 3 && guard-- > 0) {
        bool clipped = false;
        for (size_t i = 0; i < ring.size (); ++i) {
            const uint32_t a = ring[(i + ring.size () - 1) % ring.size ()], b = ring[i],
                           c = ring[(i + 1) % ring.size ()];
            if (cross (a, b, c) <= 0.0)
                continue; // reflex or flat: not an ear
            bool empty = true;
            for (uint32_t p : ring)
                if (p != a && p != b && p != c && inside (a, b, c, p)) {
                    empty = false;
                    break;
                }
            if (!empty)
                continue;
            out.insert (out.end (), { a, b, c });
            ring.erase (ring.begin () + static_cast<std::ptrdiff_t> (i));
            clipped = true;
            break;
        }
        if (!clipped)
            break; // degenerate input: draw what was found rather than loop
    }
    if (ring.size () == 3)
        out.insert (out.end (), { ring[0], ring[1], ring[2] });
    return out;
}

namespace {

void Push (std::vector<double>& points, Vec3 p)
{
    points.insert (points.end (), { p.x, p.y, p.z });
}

layers::Polyline Line (const std::vector<Vec3>& points, bool closed, uint32_t rgba, float width)
{
    layers::Polyline line;
    for (const auto& p : points)
        Push (line.points, p);
    line.closed = closed;
    line.rgba = rgba;
    line.widthPixels = width;
    return line;
}

std::vector<Vec3> Ring (const edit::Entity& e, double lift)
{
    std::vector<Vec3> out;
    for (const auto& v : e.vertices)
        out.push_back (v.p + Vec3 { 0.0, 0.0, lift });
    return out;
}

void AddSolid (const edit::Entity& e, uint32_t rgba, layers::Layer& layer)
{
    layers::Mesh mesh;
    const auto bottom = Ring (e, 0.0);
    const auto tris = TriangulateXY (bottom);
    const uint32_t n = static_cast<uint32_t> (bottom.size ());
    for (const auto& p : bottom)
        Push (mesh.points, p);
    for (const auto& p : Ring (e, e.height))
        Push (mesh.points, p);
    for (size_t t = 0; t + 2 < tris.size (); t += 3) {
        mesh.indices.insert (mesh.indices.end (), { tris[t], tris[t + 2], tris[t + 1] }); // bottom faces down
        mesh.indices.insert (mesh.indices.end (), { n + tris[t], n + tris[t + 1], n + tris[t + 2] }); // top faces up
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t j = (i + 1) % n;
        mesh.indices.insert (mesh.indices.end (), { i, j, n + j, i, n + j, n + i });
    }
    mesh.rgba = rgba;
    mesh.style.shading = layers::Shading::Ghost;
    mesh.styled = true;
    layer.meshes.push_back (std::move (mesh));
}

void AddEntity (const edit::Entity& e, uint32_t rgba, float width, layers::Layer& layer)
{
    layer.polylines.push_back (Line (Ring (e, 0.0), e.closed, rgba, width));
    if (e.kind != edit::Kind::Extrusion)
        return;
    layer.polylines.push_back (Line (Ring (e, e.height), true, rgba, width));
    for (const auto& v : e.vertices)
        layer.polylines.push_back (Line ({ v.p, v.p + Vec3 { 0.0, 0.0, e.height } }, false, rgba, width));
}

// The geometry a reference stands for, drawn in `rgba`.
void AddRef (const edit::Document& doc, const edit::Ref& ref, uint32_t rgba, const Style& style, layers::Layer& layer)
{
    const edit::Entity* e = edit::Find (doc, ref.entity);
    if (e == nullptr || !edit::Exists (doc, ref))
        return;
    switch (ref.part) {
        case edit::Part::Entity:
            AddEntity (*e, rgba, style.selectedWidth, layer);
            break;
        case edit::Part::Segment: {
            size_t from = 0, to = 0;
            edit::SegmentEnds (*e, static_cast<size_t> (edit::SegmentIndex (*e, ref.item)), from, to);
            layer.polylines.push_back (
                Line ({ e->vertices[from].p, e->vertices[to].p }, false, rgba, style.selectedWidth));
            break;
        }
        case edit::Part::Vertex: {
            layers::PointSet p;
            Vec3 at;
            edit::PointOf (doc, ref, at);
            Push (p.points, at);
            p.rgba = rgba;
            p.sizePixels = style.vertexPixels + 3.0f;
            layer.points.push_back (std::move (p));
            break;
        }
        case edit::Part::Face:
            layer.polylines.push_back (Line (edit::FacePolygon (*e, ref.item), true, rgba, style.selectedWidth));
            break;
        case edit::Part::PathPoint:
        case edit::Part::Break:
        case edit::Part::Range:
            break; // drawn with their path
    }
}

void AddDimension (const edit::Document& doc, const edit::Dimension& d, const Style& style, layers::Layer& layer)
{
    double value = 0.0;
    if (!edit::ReadDimension (doc, d, value))
        return;
    const std::string label = FormatValue (d.quantity, value);
    const edit::Entity* e = edit::Find (doc, d.ref.entity);
    Vec3 a, b;
    if (d.ref.part == edit::Part::Segment) {
        size_t from = 0, to = 0;
        edit::SegmentEnds (*e, static_cast<size_t> (edit::SegmentIndex (*e, d.ref.item)), from, to);
        a = e->vertices[from].p;
        b = e->vertices[to].p;
    }
    if (d.quantity == edit::Quantity::Length || d.quantity == edit::Quantity::Distance) {
        layers::Dimension dim;
        for (int i = 0; i < 3; ++i) {
            dim.from[i] = i == 0 ? a.x : i == 1 ? a.y : a.z;
            dim.to[i] = i == 0 ? b.x : i == 1 ? b.y : b.z;
        }
        dim.text = label;
        dim.rgba = style.text;
        dim.textSizePixels = style.textPixels;
        dim.behind = layers::Behind::Show;
        layer.dimensions.push_back (std::move (dim));
        return;
    }
    layers::Text text;
    text.text = label;
    text.sizePixels = style.textPixels;
    text.rgba = style.text;
    text.behind = layers::Behind::Show;
    Vec3 at;
    edit::PointOf (doc, d.ref, at);
    if (d.quantity == edit::Quantity::Slope) {
        // A slope arrow points downhill over the middle of the segment.
        const bool down = b.z <= a.z;
        const Vec3 high = down ? a : b, low = down ? b : a;
        layers::Polyline arrow =
            Line ({ edit::Lerp (high, low, 0.35), edit::Lerp (high, low, 0.65) }, false, style.text, 1.5f);
        arrow.endArrow = layers::Terminator::Arrow;
        arrow.behind = layers::Behind::Show;
        layer.polylines.push_back (std::move (arrow));
        text.offsetPixels[1] = -14.0f;
    }
    else if (d.quantity == edit::Quantity::Angle) {
        at = a;
        text.offsetPixels[0] = 18.0f;
        text.offsetPixels[1] = 14.0f;
    }
    else if (d.quantity == edit::Quantity::Height && e != nullptr) {
        at = e->vertices.front ().p + Vec3 { 0.0, 0.0, e->height * 0.5 };
        text.offsetPixels[0] = 18.0f;
    }
    else
        text.offsetPixels[1] = -14.0f;
    text.at[0] = at.x;
    text.at[1] = at.y;
    text.at[2] = at.z;
    layer.texts.push_back (std::move (text));
}

void AddSnap (const edit::SnapResult& snap, const Style& style, layers::Layer& layer)
{
    if (!snap.snapped)
        return;
    for (const auto& g : snap.guides) {
        layers::Polyline guide = Line ({ g.from, g.to }, false, style.guide, 1.25f);
        guide.dashMetres = { 0.25f, 0.15f };
        guide.behind = layers::Behind::Show;
        layer.polylines.push_back (std::move (guide));
    }
    layers::PointSet marker;
    Push (marker.points, snap.point);
    marker.rgba = style.snap;
    marker.sizePixels = 9.0f;
    layer.points.push_back (std::move (marker));
    if (!snap.hint.empty ()) {
        layers::Text hint;
        hint.text = snap.hint;
        hint.at[0] = snap.point.x;
        hint.at[1] = snap.point.y;
        hint.at[2] = snap.point.z;
        hint.offsetPixels[0] = 14.0f;
        hint.offsetPixels[1] = -14.0f;
        hint.align = layers::Align::Left;
        hint.sizePixels = style.textPixels;
        hint.rgba = style.snap;
        hint.behind = layers::Behind::Show;
        layer.texts.push_back (std::move (hint));
    }
}

void AddGizmo (const GizmoView& g, const Style& style, layers::Layer& layer)
{
    if (!g.shown)
        return;
    const auto handles = edit::Handles (g.mode);
    for (const auto& h : handles) {
        const bool active = static_cast<int> (h.index) == g.active;
        const uint32_t rgba = active ? style.activeHandle : h.axis >= 0 && h.axis < 3 ? style.axis[h.axis] : style.text;
        for (const auto& shape : edit::HandleGeometry (g.frame, h, g.size)) {
            if (shape.points.size () == 1) {
                layers::PointSet centre;
                Push (centre.points, shape.points[0]);
                centre.rgba = rgba;
                centre.sizePixels = 10.0f;
                layer.points.push_back (std::move (centre));
                continue;
            }
            layers::Polyline line = Line (shape.points, shape.closed, rgba, active ? 3.5f : 2.5f);
            if (h.kind == edit::HandleKind::Axis || h.kind == edit::HandleKind::Face)
                line.endArrow = layers::Terminator::Arrow;
            line.behind = layers::Behind::Show;
            layer.polylines.push_back (std::move (line));
        }
    }
}

} // namespace

layers::Layer Build (const edit::Document& doc, const Feedback& f, const Style& style, const std::string& name)
{
    layers::Layer layer;
    layer.name = name;
    layer.lifetime = layers::LayerLifetime::Command;
    layer.occlusion = layers::Behind::Fade; // the edit stays readable behind the building
    layers::PointSet vertices;
    vertices.rgba = style.geometry;
    vertices.sizePixels = style.vertexPixels;
    for (const auto& e : doc.entities) {
        AddEntity (e, style.geometry, style.lineWidth, layer);
        if (e.kind == edit::Kind::Extrusion)
            AddSolid (e, style.solid, layer);
        for (const auto& v : e.vertices)
            Push (vertices.points, v.p);
    }
    if (!vertices.points.empty ())
        layer.points.push_back (std::move (vertices));
    if (f.selection != nullptr) {
        for (const auto& ref : f.selection->Items ())
            AddRef (doc, ref, style.selected, style, layer);
        if (f.selection->hover.entity != edit::kNoId && !f.selection->Contains (f.selection->hover))
            AddRef (doc, f.selection->hover, style.hover, style, layer);
    }
    for (const auto& d : f.dimensions)
        AddDimension (doc, d, style, layer);
    if (f.snap != nullptr)
        AddSnap (*f.snap, style, layer);
    AddGizmo (f.gizmo, style, layer);
    return layer;
}

} // namespace geomsrv::archviz::editlayers
