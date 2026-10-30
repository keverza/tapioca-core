#include "ArchViz/EditLayers.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
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
        case edit::Quantity::Factor:
            std::snprintf (text, sizeof text, "x%.3f", value);
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

std::vector<LiveLabel> LiveDimensions (const edit::Document& doc, const edit::Ref& ref, bool fromEnd)
{
    std::vector<LiveLabel> out;
    for (auto d : edit::DimensionsFor (doc, ref)) {
        d.fromEnd = fromEnd;
        double value = 0.0;
        if (!edit::ReadDimension (doc, d, value))
            continue;
        LiveLabel l;
        l.value = d;
        l.quantity = d.quantity;
        edit::PointOf (doc, d.ref, l.at);
        bool fixedIsStart = true;
        if (d.quantity == edit::Quantity::Length || d.quantity == edit::Quantity::Angle)
            l.ends = edit::SegmentEndsFor (doc, d, l.fixed, l.moving, fixedIsStart);
        const edit::Entity* e = edit::Find (doc, d.ref.entity);
        const bool segment = e != nullptr && d.ref.part == edit::Part::Segment;
        const bool onPath = edit::FindPath (doc, d.ref.entity) != nullptr;
        size_t from = 0, to = 0;
        if (segment)
            edit::SegmentEnds (*e, static_cast<size_t> (edit::SegmentIndex (*e, d.ref.item)), from, to);
        switch (d.quantity) {
            case edit::Quantity::Length:
            case edit::Quantity::Distance:
                if (segment && edit::SegmentCurved (*e, static_cast<size_t> (edit::SegmentIndex (*e, d.ref.item)))) {
                    // Along a curve: the label at its middle, no straight dimension line.
                    l.at = edit::SegmentPoint (*e, static_cast<size_t> (edit::SegmentIndex (*e, d.ref.item)), 0.5);
                    l.ends = false;
                    l.text = "arc " + FormatValue (d.quantity, value);
                    break;
                }
                if (segment) {
                    l.line = true;
                    l.from = e->vertices[from].p;
                    l.to = e->vertices[to].p;
                    l.at = edit::Lerp (l.from, l.to, 0.5);
                }
                l.text = FormatValue (d.quantity, value);
                break;
            case edit::Quantity::Slope:
                if (onPath || std::abs (value) < 0.05)
                    continue; // flat says nothing; a range's crossfall is on the layer
                l.text = FormatValue (d.quantity, value);
                break;
            case edit::Quantity::Angle:
                if (l.ends)
                    l.at = l.fixed; // the arc is drawn about the end that stays
                l.text = FormatValue (d.quantity, value);
                break;
            case edit::Quantity::Height:
                if (e != nullptr && !e->vertices.empty ())
                    l.at = e->vertices.front ().p + Vec3 { 0.0, 0.0, e->height * 0.5 };
                l.text = "H " + FormatValue (d.quantity, value);
                break;
            case edit::Quantity::Width:
                l.text = "W " + FormatValue (d.quantity, value);
                break;
            case edit::Quantity::Radius:
                if (d.ref.part == edit::Part::Vertex && !(value > 0.0))
                    continue; // a sharp corner: its grip says it can be rounded
                l.text = "R " + FormatValue (d.quantity, value);
                break;
            case edit::Quantity::Elevation:
            case edit::Quantity::Z:
                if (onPath)
                    continue; // a grade break's height is on the layer
                l.text = "Z " + FormatValue (d.quantity, value);
                break;
            default:
                continue; // plan coordinates are the tracker's
        }
        out.push_back (std::move (l));
    }
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

// The outline as drawn (its live corners rounded), lifted.
std::vector<Vec3> Ring (const edit::Entity& e, double lift)
{
    std::vector<Vec3> out = edit::Shape (e);
    for (auto& p : out)
        p = p + Vec3 { 0.0, 0.0, lift };
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
    for (size_t i = 0; i < e.vertices.size (); ++i) // a rounded corner has no edge up its side
        if (!(edit::DrawnRadius (e, i) > 0.0))
            layer.polylines.push_back (
                Line ({ e.vertices[i].p, e.vertices[i].p + Vec3 { 0.0, 0.0, e.height } }, false, rgba, width));
}

std::string Percent (double value)
{
    return FormatValue (edit::Quantity::Slope, value);
}

layers::Text Label (const std::string& text, Vec3 at, const Style& style, float dy, uint32_t rgba)
{
    layers::Text t;
    t.text = text;
    t.at[0] = at.x;
    t.at[1] = at.y;
    t.at[2] = at.z;
    t.offsetPixels[1] = dy;
    t.sizePixels = style.textPixels;
    t.rgba = rgba;
    t.backgroundRgba = style.paper;
    t.haloRgba = 0;
    t.behind = layers::Behind::Show;
    return t;
}

layers::Polyline Arrow (Vec3 from, Vec3 to, uint32_t rgba)
{
    layers::Polyline a = Line ({ from, to }, false, rgba, 1.5f);
    a.endArrow = layers::Terminator::Arrow;
    a.behind = layers::Behind::Show;
    return a;
}

// The centre at a station, through the outline's own samples.
bool CentreAt (const edit::PathOutline& o, double station, Vec3& out)
{
    for (size_t i = 0; i + 1 < o.stations.size (); ++i)
        if (station <= o.stations[i + 1] + 1.0e-9) {
            const double span = o.stations[i + 1] - o.stations[i];
            out = Lerp (o.centre[i], o.centre[i + 1], span > 0.0 ? (station - o.stations[i]) / span : 0.0);
            return true;
        }
    return false;
}

bool AnnotationStation (const edit::Document& doc, const edit::Path& path, double from, double to, double preferred,
                        double& station, double* halfSpan = nullptr)
{
    double best = 1e100;
    bool found = false;
    for (const auto& span : edit::PathAnnotationSpans (doc.paths, path.id, from, to)) {
        if (span.to - span.from < 0.1)
            continue;
        const double at =
            preferred > span.from + 0.05 && preferred < span.to - 0.05 ? preferred : (span.from + span.to) * 0.5;
        if (std::abs (at - preferred) >= best)
            continue;
        best = std::abs (at - preferred);
        station = at;
        if (halfSpan)
            *halfSpan = 0.8 * std::min (at - span.from, span.to - at);
        found = true;
    }
    return found;
}

bool FrameAt (const edit::Path& path, double station, edit::StationFrame& frame)
{
    edit::AlignmentGeometry g;
    std::string error;
    return edit::BuildAlignment (path.points, g, error) && edit::Evaluate (path, g, station, frame);
}

// A path's labels: each grade break's height, each tangent's grade over its middle, each
// range's crossfall on its falling side.
void PathMarkLabels (const edit::Document& doc, const edit::Path& path, const edit::PathOutline& o, const Style& style,
                     std::vector<MarkLabel>& out)
{
    const auto leftAt = [&] (double station) {
        size_t k = 0;
        while (k + 1 < o.stations.size () && o.stations[k] < station)
            ++k;
        Vec3 n;
        return edit::Normalize (o.left[k] - o.right[k], n) ? Vec3 { n.x, n.y, 0.0 } : Vec3 { 0.0, 1.0, 0.0 };
    };
    const auto tangentAt = [&] (double station) {
        const Vec3 n = leftAt (station);
        return Vec3 { n.y, -n.x, 0.0 };
    };
    for (size_t i = 0; i < path.profile.size () && i < o.breaks.size (); ++i) {
        const auto& b = path.profile[i];
        if (b.crossing != edit::kNoId)
            continue; // its exact height is not a measurement of an arm's run
        MarkLabel l;
        l.text = FormatValue (edit::Quantity::Elevation, b.z);
        if (b.fixed)
            l.text += " fixed";
        else if (b.crossing != edit::kNoId)
            l.text += " junction";
        l.at = o.breaks[i];
        l.side = leftAt (b.station) * -1.0;
        l.along = tangentAt (b.station);
        l.rgba = b.fixed ? style.fixedBreak : b.crossing != edit::kNoId ? style.junctionBreak : style.text;
        l.dy = 16.0f;
        l.typed = true;
        l.value = { { path.id, edit::Part::Break, b.id }, edit::Quantity::Elevation };
        out.push_back (std::move (l));
    }
    for (size_t i = 0; i + 1 < path.profile.size (); ++i) {
        const double s0 = path.profile[i].station, s1 = path.profile[i + 1].station;
        MarkLabel l;
        double station = 0;
        if (!AnnotationStation (doc, path, s0 + path.profile[i].curve * 0.5, s1 - path.profile[i + 1].curve * 0.5,
                                (s0 + s1) * 0.5, station) ||
            !CentreAt (o, station, l.at))
            continue;
        l.side = leftAt (station);
        l.along = tangentAt (station);
        l.text = Percent (std::abs (edit::TangentGrade (path.profile, i) * 100.0));
        l.rgba = style.text;
        l.dy = -18.0f;
        l.typed = true; // the tangent from its first break on
        l.value = { { path.id, edit::Part::Break, path.profile[i].id }, edit::Quantity::Slope };
        out.push_back (std::move (l));
    }
    for (size_t r = 0; r < path.sections.size (); ++r) {
        const auto& section = path.sections[r];
        const double end = r + 1 < path.sections.size () ? path.sections[r + 1].station : o.stations.back ();
        double station = 0;
        edit::StationFrame frame;
        if (!AnnotationStation (doc, path, section.station, end, (section.station + end) * 0.5, station) ||
            !FrameAt (path, station, frame))
            continue;
        const Vec3 c = frame.centre, edge = section.fall == edit::Fall::Right ? frame.rightEdge : frame.leftEdge;
        MarkLabel l;
        if (!edit::Normalize (Vec3 { edge.x - c.x, edge.y - c.y, 0.0 }, l.side))
            l.side = { 0.0, 1.0, 0.0 };
        l.along = l.side;
        l.at = Lerp (c, edge, 0.55);
        l.text = Percent (section.crossfall);
        l.rgba = style.fall;
        l.dy = 16.0f;
        l.typed = true;
        l.value = { { path.id, edit::Part::Range, section.id }, edit::Quantity::Slope };
        out.push_back (std::move (l));
    }
}

// A path's dimensions for the view: each range's width across it, a third of the way along
// (clear of its crossfall), and each bend's radius from its arc's centre to its middle.
void PathDimensionLabels (const edit::Document& doc, const edit::Path& path, const edit::PathOutline& o,
                          const Style& style, std::vector<MarkLabel>& out)
{
    for (size_t r = 0; r < path.sections.size (); ++r) {
        const auto& section = path.sections[r];
        const double end = r + 1 < path.sections.size () ? path.sections[r + 1].station : o.stations.back ();
        double station = 0;
        edit::StationFrame frame;
        if (!AnnotationStation (doc, path, section.station, end, section.station + (end - section.station) / 3.0,
                                station) ||
            !FrameAt (path, station, frame))
            continue;
        MarkLabel l;
        l.line = true;
        l.from = frame.rightEdge;
        l.to = frame.leftEdge;
        l.at = frame.centre;
        if (!edit::Normalize (Vec3 { l.to.x - l.from.x, l.to.y - l.from.y, 0.0 }, l.along))
            continue;
        l.side = { l.along.y, -l.along.x, 0.0 }; // ahead along the path
        l.extent = section.widthLeft + section.widthRight;
        l.text = FormatValue (edit::Quantity::Width, l.extent);
        l.rgba = style.dimension;
        l.typed = true;
        l.value = { { path.id, edit::Part::Range, section.id }, edit::Quantity::Width };
        out.push_back (std::move (l));
    }
    edit::AlignmentGeometry g;
    std::string error;
    if (!edit::BuildAlignment (path.points, g, error))
        return;
    for (size_t i = 1; i + 1 < path.points.size () && i < g.pointStations.size (); ++i) {
        edit::Placement p;
        Vec3 mid;
        if (!(path.points[i].radius > 0.0) || !edit::Locate (g, g.pointStations[i], p) ||
            std::abs (p.curvature) < 1.0e-9 || !CentreAt (o, g.pointStations[i], mid))
            continue;
        MarkLabel l;
        l.line = l.radial = true;
        l.to = { p.point.x, p.point.y, mid.z }; // on the arc, not its sampled chord
        l.from = l.to + p.left * (1.0 / p.curvature);
        l.at = Lerp (l.from, l.to, 0.5);
        if (!edit::Normalize (Vec3 { l.to.x - l.from.x, l.to.y - l.from.y, 0.0 }, l.along))
            continue;
        l.side = p.tangent;
        l.extent = path.points[i].radius;
        l.text = "R " + FormatValue (edit::Quantity::Radius, l.extent);
        l.rgba = style.dimension;
        l.typed = true;
        l.value = { { path.id, edit::Part::PathPoint, path.points[i].id }, edit::Quantity::Radius };
        out.push_back (std::move (l));
    }
}

// A path's marks over its surface: control polygon and points, grade breaks with their
// heights, grade arrows and percentages, crossfall arrows.
void AddPathMarks (const edit::Document& doc, const edit::Path& path, const edit::PathOutline& o, const Style& style,
                   layers::Layer& layer, bool labels)
{
    // Control polygon and points: the plan handles.
    layers::Polyline polygon = Line (o.points, false, style.controlPolygon, 1.0f);
    polygon.dashMetres = { 0.4f, 0.25f };
    layer.polylines.push_back (std::move (polygon));
    layers::PointSet points;
    points.rgba = style.geometry;
    points.sizePixels = style.vertexPixels;
    for (const auto& p : o.points)
        Push (points.points, p);
    layer.points.push_back (std::move (points));

    // Grade breaks with their elevations; fixed ones in their own colour.
    layers::PointSet free, fixed, junction;
    free.rgba = style.gradeBreak;
    fixed.rgba = style.fixedBreak;
    junction.rgba = style.junctionBreak;
    free.sizePixels = fixed.sizePixels = junction.sizePixels = style.vertexPixels + 1.0f;
    for (size_t i = 0; i < path.profile.size () && i < o.breaks.size (); ++i) {
        const auto& b = path.profile[i];
        Push (b.fixed ? fixed.points : b.crossing != edit::kNoId ? junction.points : free.points, o.breaks[i]);
    }
    for (auto* set : { &free, &fixed, &junction })
        if (!set->points.empty ())
            layer.points.push_back (std::move (*set));

    // A downhill arrow and the grade over the middle of each tangent.
    for (size_t i = 0; i + 1 < path.profile.size (); ++i) {
        const double s0 = path.profile[i].station, s1 = path.profile[i + 1].station;
        const double grade = edit::TangentGrade (path.profile, i) * 100.0;
        Vec3 a, b;
        double station = 0, halfSpan = 0;
        if (!AnnotationStation (doc, path, s0 + path.profile[i].curve * 0.5, s1 - path.profile[i + 1].curve * 0.5,
                                (s0 + s1) * 0.5, station, &halfSpan))
            continue;
        halfSpan = std::min (halfSpan, (s1 - s0) * 0.1);
        if (!CentreAt (o, station - halfSpan, a) || !CentreAt (o, station + halfSpan, b))
            continue;
        if (std::abs (grade) > 1.0e-9) {
            std::vector<Vec3> run { a };
            for (size_t k = 0; k < o.stations.size (); ++k)
                if (o.stations[k] > station - halfSpan + 1e-9 && o.stations[k] < station + halfSpan - 1e-9)
                    run.push_back (o.centre[k]);
            run.push_back (b);
            if (grade > 0.0)
                std::reverse (run.begin (), run.end ());
            auto arrow = Line (run, false, style.text, 1.5f);
            arrow.endArrow = layers::Terminator::Arrow;
            arrow.behind = layers::Behind::Show;
            layer.polylines.push_back (std::move (arrow));
        }
    }

    // Crossfall: arrows from the high side towards the falling edge(s) in each range.
    for (size_t r = 0; r < path.sections.size (); ++r) {
        const auto& section = path.sections[r];
        const double end = r + 1 < path.sections.size () ? path.sections[r + 1].station : o.stations.back ();
        double station = 0;
        edit::StationFrame frame;
        if (!AnnotationStation (doc, path, section.station, end, (section.station + end) * 0.5, station) ||
            !FrameAt (path, station, frame))
            continue;
        const Vec3 c = frame.centre, l = frame.leftEdge, rt = frame.rightEdge;
        const auto toward = [&] (Vec3 edge) { return Lerp (c, edge, 0.8); };
        if (section.fall == edit::Fall::Crown) {
            layer.polylines.push_back (Arrow (c, toward (l), style.fall));
            layer.polylines.push_back (Arrow (c, toward (rt), style.fall));
        }
        else if (section.fall == edit::Fall::Left)
            layer.polylines.push_back (Arrow (toward (rt), toward (l), style.fall));
        else
            layer.polylines.push_back (Arrow (toward (l), toward (rt), style.fall));
    }
    if (!labels)
        return;
    std::vector<MarkLabel> marks;
    PathMarkLabels (doc, path, o, style, marks);
    for (const auto& l : marks)
        layer.texts.push_back (Label (l.text, l.at, style, l.dy, l.rgba));
}

// The paths paved as one area: crossings and junctions united, corners rounded by their
// kerb radii, the surface triangulated with the paths' heights.
bool AddPaved (const edit::PavedArea& paved, const Style& style, layers::Layer& layer)
{
    if (paved.rings.empty ())
        return false;
    if (!paved.triangles.empty ()) {
        layers::Mesh surface;
        for (const auto& p : paved.vertices)
            Push (surface.points, p);
        surface.indices = paved.triangles;
        surface.rgba = style.pathSurface;
        surface.style.shading = layers::Shading::Flat;
        surface.styled = true;
        layer.meshes.push_back (std::move (surface));
    }
    for (const auto& ring : paved.rings)
        layer.polylines.push_back (Line (ring, true, style.pathEdge, 1.5f));
    return true;
}

void AddPath (const edit::Document& doc, const edit::Path& path, const Style& style, layers::Layer& layer, bool paved,
              bool labels, bool detail)
{
    if (paved && !detail)
        return;
    edit::PathOutline o;
    std::string error;
    if (!edit::Outline (path, style.pathStep, o, error) || o.stations.size () < 2)
        return;
    if (paved) {
        layer.polylines.push_back (Line (o.centre, false, style.geometry, style.lineWidth));
        AddPathMarks (doc, path, o, style, layer, labels);
        return;
    }
    // The walking surface as a strip between the edges.
    layers::Mesh surface;
    const uint32_t n = static_cast<uint32_t> (o.stations.size ());
    for (uint32_t i = 0; i < n; ++i) {
        Push (surface.points, o.left[i]);
        Push (surface.points, o.right[i]);
    }
    for (uint32_t i = 0; i + 1 < n; ++i) {
        const uint32_t l0 = 2 * i, r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
        surface.indices.insert (surface.indices.end (), { l0, r0, r1, l0, r1, l1 });
    }
    surface.rgba = style.pathSurface;
    surface.style.shading = layers::Shading::Flat;
    surface.styled = true;
    layer.meshes.push_back (std::move (surface));
    if (detail)
        layer.polylines.push_back (Line (o.centre, false, style.geometry, style.lineWidth));
    layer.polylines.push_back (Line (o.left, false, style.pathEdge, 1.5f));
    layer.polylines.push_back (Line (o.right, false, style.pathEdge, 1.5f));
    if (detail)
        AddPathMarks (doc, path, o, style, layer, labels);
}

// A selected or hovered part of a path.
// `paved`: the paths' one surface, which a range's edges are drawn on (OntoPavedSurface).
void AddPathRef (const edit::Path& path, const edit::Ref& ref, uint32_t rgba, const Style& style, layers::Layer& layer,
                 const edit::PavedArea* paved)
{
    edit::PathOutline o;
    std::string error;
    if (!edit::Outline (path, style.pathStep, o, error))
        return;
    const auto marker = [&] (Vec3 at) {
        layers::PointSet p;
        Push (p.points, at);
        p.rgba = rgba;
        p.sizePixels = style.vertexPixels + 4.0f;
        layer.points.push_back (std::move (p));
    };
    switch (ref.part) {
        case edit::Part::Entity:
            layer.polylines.push_back (Line (o.centre, false, rgba, style.selectedWidth));
            break;
        case edit::Part::PathPoint:
            for (size_t i = 0; i < path.points.size () && i < o.points.size (); ++i)
                if (path.points[i].id == ref.item)
                    marker (o.points[i]);
            break;
        case edit::Part::Break:
            for (size_t i = 0; i < path.profile.size () && i < o.breaks.size (); ++i)
                if (path.profile[i].id == ref.item)
                    marker (o.breaks[i]);
            break;
        case edit::Part::Range: {
            std::vector<Vec3> left, right;
            for (size_t i = 0; i < o.stations.size (); ++i)
                if (o.range[i] == ref.item) {
                    left.push_back (o.left[i]);
                    right.push_back (o.right[i]);
                }
            if (left.size () >= 2) {
                if (paved != nullptr)
                    edit::OntoPavedSurface (*paved, left), edit::OntoPavedSurface (*paved, right);
                layer.polylines.push_back (Line (left, false, rgba, style.selectedWidth));
                layer.polylines.push_back (Line (right, false, rgba, style.selectedWidth));
            }
            break;
        }
        default:
            break;
    }
}

// The geometry a reference stands for, drawn in `rgba`.
void AddRef (const edit::Document& doc, const edit::Ref& ref, uint32_t rgba, const Style& style, layers::Layer& layer,
             const edit::PavedArea* paved)
{
    if (const edit::Path* path = edit::FindPath (doc, ref.entity)) {
        AddPathRef (*path, ref, rgba, style, layer, paved);
        return;
    }
    const edit::Entity* e = edit::Find (doc, ref.entity);
    if (e == nullptr || !edit::Exists (doc, ref))
        return;
    switch (ref.part) {
        case edit::Part::Entity:
            AddEntity (*e, rgba, style.selectedWidth, layer);
            break;
        case edit::Part::Segment: // as drawn: a curve along its curve
            layer.polylines.push_back (
                Line (edit::SegmentPoints (*e, static_cast<size_t> (edit::SegmentIndex (*e, ref.item))), false, rgba,
                      style.selectedWidth));
            break;
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
        case edit::Part::LandscapeFace:
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
        dim.rgba = style.dimension;
        dim.textRgba = style.text;
        dim.haloRgba = style.paper;
        dim.haloPixels = 2.0f;
        dim.textSizePixels = style.textPixels;
        dim.behind = layers::Behind::Show;
        layer.dimensions.push_back (std::move (dim));
        return;
    }
    layers::Text text;
    text.text = label;
    text.sizePixels = style.textPixels;
    text.rgba = style.text;
    text.backgroundRgba = style.paper;
    text.haloRgba = 0;
    text.behind = layers::Behind::Show;
    Vec3 at;
    edit::PointOf (doc, d.ref, at);
    if (d.quantity == edit::Quantity::Slope && d.ref.part == edit::Part::Segment) {
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
        hint.rgba = style.text;
        hint.backgroundRgba = style.paper;
        hint.haloRgba = 0;
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

std::vector<MarkLabel> PathLabels (const edit::Document& doc, const Style& style)
{
    std::vector<MarkLabel> out;
    for (const auto& path : doc.paths) {
        edit::PathOutline o;
        std::string error;
        if (edit::Shown (doc, path.member) && edit::Outline (path, style.pathStep, o, error) &&
            o.stations.size () >= 2) {
            PathMarkLabels (doc, path, o, style, out);
            PathDimensionLabels (doc, path, o, style, out);
        }
    }
    // Each rounded kerb corner's radius, from its fillet's centre to its middle.
    for (const auto& c : edit::Pave (doc.paths, style.pathStep, false).corners) {
        if (c.key.radius < 0.005 || !edit::Shown (doc, c.path))
            continue; // "R 0.00 m" says nothing, and a hidden path says nothing at all
        const double r = c.key.radius;
        if (!(r > 0.0) || !(c.half > 1.0e-6))
            continue;
        MarkLabel l;
        l.line = l.radial = true;
        l.to = edit::KerbGripAt (c, 0.0);
        l.from = c.at + c.bisector * (r / std::sin (c.half));
        l.at = Lerp (l.from, l.to, 0.5);
        l.along = c.bisector * -1.0;
        l.side = { -c.bisector.y, c.bisector.x, 0.0 };
        l.extent = r;
        l.text = "R " + FormatValue (edit::Quantity::Radius, r);
        l.rgba = style.dimension;
        l.typed = true;
        l.kerbPath = c.path;
        l.kerb = c.key;
        l.kerb.radius = c.requestedRadius; // typing edits intent, never the fitted display value
        l.limitedRadius = c.Limited ();
        if (l.limitedRadius)
            l.textRgba = style.limitedRadius;
        out.push_back (std::move (l));
    }
    return out;
}

namespace {
// What the layer held before a part of the scene was added, so that part can be faded alone.
struct Mark {
    size_t polylines = 0, points = 0, meshes = 0, texts = 0, dimensions = 0;
};
Mark MarkOf (const layers::Layer& layer)
{
    return { layer.polylines.size (), layer.points.size (), layer.meshes.size (), layer.texts.size (),
             layer.dimensions.size () };
}
uint32_t Faded (uint32_t rgba)
{
    return (rgba & 0xFFFFFF00u) | ((rgba & 0xFFu) / 4u);
}
void Fade (layers::Layer& layer, const Mark& from)
{
    for (size_t i = from.polylines; i < layer.polylines.size (); ++i)
        layer.polylines[i].rgba = Faded (layer.polylines[i].rgba);
    for (size_t i = from.points; i < layer.points.size (); ++i)
        layer.points[i].rgba = Faded (layer.points[i].rgba);
    for (size_t i = from.meshes; i < layer.meshes.size (); ++i) {
        layer.meshes[i].rgba = Faded (layer.meshes[i].rgba);
        for (auto& c : layer.meshes[i].vertexRgba)
            c = Faded (c);
    }
    for (size_t i = from.texts; i < layer.texts.size (); ++i)
        layer.texts[i].rgba = Faded (layer.texts[i].rgba);
    for (size_t i = from.dimensions; i < layer.dimensions.size (); ++i)
        layer.dimensions[i].rgba = Faded (layer.dimensions[i].rgba);
}

// The colour an object's lines are drawn in: its layer's, or the style's when the layer has none.
uint32_t LineColour (const edit::Document& doc, const edit::Membership& m, uint32_t fallback)
{
    const edit::Layer* on = edit::FindLayer (doc, m.layer);
    return on != nullptr && on->rgba != 0 ? on->rgba : fallback;
}
} // namespace

layers::Layer Build (const edit::Document& doc, const Feedback& f, const Style& style, const std::string& name)
{
    layers::Layer layer;
    layer.name = name;
    layer.lifetime = layers::LayerLifetime::Command;
    layer.occlusion = layers::Behind::Fade; // the edit stays readable behind the building
    const auto inFocus = [&f] (edit::Id id) {
        return f.focus == nullptr || std::find (f.focus->begin (), f.focus->end (), id) != f.focus->end ();
    };
    layers::PointSet vertices, faded;
    vertices.rgba = style.geometry;
    vertices.sizePixels = style.vertexPixels;
    faded = vertices;
    faded.rgba = Faded (style.geometry);
    for (const auto& e : doc.entities) {
        if (!edit::Shown (doc, e.member))
            continue;
        const Mark before = MarkOf (layer);
        const bool source = f.landscape && doc.landscape &&
                            std::any_of (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                         [&] (const auto& s) { return s.entityId == e.id && !s.deleted; });
        // A layer with its own colour draws its objects' lines in it.
        if (!source)
            AddEntity (e, LineColour (doc, e.member, style.geometry), style.lineWidth, layer);
        if (e.kind == edit::Kind::Extrusion)
            AddSolid (e, style.solid, layer);
        for (const auto& v : e.vertices)
            Push ((inFocus (e.id) ? vertices : faded).points, v.p);
        if (!inFocus (e.id))
            Fade (layer, before);
    }
    if (!vertices.points.empty ())
        layer.points.push_back (std::move (vertices));
    if (!faded.points.empty ())
        layer.points.push_back (std::move (faded));
    // The landscape, the shared surface and the contours are no group's: faded inside one.
    const Mark shared = MarkOf (layer);
    if (f.landscape)
        AddLandscape (doc, f, style, layer);
    // The paths' one surface, paved once for the fill, its outline and the parts drawn on it. The
    // cached paving holds every path, so with any of them hidden the shown ones are paved here.
    std::vector<edit::Path> shown;
    for (const auto& p : doc.paths)
        if (edit::Shown (doc, p.member))
            shown.push_back (p);
    const edit::PavedArea* cached = shown.size () == doc.paths.size () ? f.paved : nullptr;
    const edit::PavedArea built = cached || shown.empty () ? edit::PavedArea {} : edit::Pave (shown, style.pathStep);
    const edit::PavedArea* surface = shown.empty () ? nullptr : cached ? cached : &built;
    const bool paved = surface != nullptr && AddPaved (*surface, style, layer);
    if (f.contours != nullptr)
        for (const auto& c : *f.contours)
            layer.polylines.push_back (Line (c.points, false, style.contour, 1.0f));
    if (f.focus != nullptr)
        Fade (layer, shared);
    const auto quiet = [&] (edit::Id id) {
        return f.quietPaths && std::find (f.quietPaths->begin (), f.quietPaths->end (), id) != f.quietPaths->end ();
    };
    for (const auto& p : shown) {
        Style own = style; // the path's centre line and points in its layer's colour
        own.geometry = LineColour (doc, p.member, style.geometry);
        const Mark before = MarkOf (layer);
        AddPath (doc, p, own, layer, paved, !f.labelsInView, !quiet (p.id));
        if (!inFocus (p.id))
            Fade (layer, before);
    }
    if (f.selection != nullptr) {
        for (const auto& ref : f.selection->Items ())
            if (!quiet (ref.entity))
                AddRef (doc, ref, style.selected, style, layer, paved ? surface : nullptr);
        if (f.selection->hover.entity != edit::kNoId && !f.selection->Contains (f.selection->hover) &&
            !quiet (f.selection->hover.entity))
            AddRef (doc, f.selection->hover, style.hover, style, layer, paved ? surface : nullptr);
    }
    for (const auto& d : f.dimensions)
        if (!quiet (d.ref.entity))
            AddDimension (doc, d, style, layer);
    if (f.snap != nullptr)
        AddSnap (*f.snap, style, layer);
    AddGizmo (f.gizmo, style, layer);
    return layer;
}

} // namespace geomsrv::archviz::editlayers
