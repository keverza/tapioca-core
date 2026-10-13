// ArchViz/OverlayHover -- see the header.

#include "ArchViz/OverlayHover.hpp"
#include "ArchViz/OverlayHoverIndex.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlayhover {

namespace {

namespace layers = overlaylayers;

bool SaysSomething (const layers::Mesh& mesh)
{
    return !mesh.hoverTitle.empty () || !mesh.hoverRows.empty () || !mesh.values.empty () || !mesh.hoverCells.empty ();
}

// The point's barycentric weights in the triangle a, b, c (x, y), or false outside it -- a
// point on an edge is inside, so two triangles sharing it both have it.
bool Weights (double x, double y, const double* a, const double* b, const double* c, double w[3])
{
    const double d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
    if (std::fabs (d) < 1e-18)
        return false; // no area in plan: a vertical face is a line here
    w[0] = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
    w[1] = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
    w[2] = 1.0 - w[0] - w[1];
    constexpr double kEdge = -1e-9;
    return w[0] >= kEdge && w[1] >= kEdge && w[2] >= kEdge;
}

std::string Number (double value, uint32_t decimals)
{
    char text[64] = {};
    std::snprintf (text, sizeof (text), "%.*f", int ((std::min) (decimals, 6u)), value);
    return text;
}

uint32_t RampColor (const layers::Mesh& mesh, double value)
{
    if (mesh.colormap.stops.empty ())
        return 0xFFFFFFFFu;
    double minimum = mesh.colormap.min, maximum = mesh.colormap.max;
    if (mesh.colormap.autoRange && !mesh.values.empty ()) {
        const auto range = std::minmax_element (mesh.values.begin (), mesh.values.end ());
        minimum = *range.first;
        maximum = *range.second;
    }
    if (!(maximum > minimum))
        maximum = minimum + 1.0;
    const float t = float (std::clamp ((value - minimum) / (maximum - minimum), 0.0, 1.0));
    uint32_t rgba = mesh.colormap.stops.front ().rgba;
    if (t >= mesh.colormap.stops.back ().at)
        rgba = mesh.colormap.stops.back ().rgba;
    else if (t > mesh.colormap.stops.front ().at) {
        for (size_t i = 1; i < mesh.colormap.stops.size (); ++i) {
            if (t > mesh.colormap.stops[i].at)
                continue;
            const layers::ColourStop& a = mesh.colormap.stops[i - 1];
            const layers::ColourStop& b = mesh.colormap.stops[i];
            const float span = b.at - a.at;
            const float fraction = span > 1e-6f ? (t - a.at) / span : 1.0f;
            rgba = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                const float left = float ((a.rgba >> shift) & 0xFFu);
                const float right = float ((b.rgba >> shift) & 0xFFu);
                rgba |= uint32_t (std::lround (left + (right - left) * fraction)) << shift;
            }
            break;
        }
    }
    // A fully opaque, brighter shade keeps the heatmap's hue while reading over its fill.
    for (int shift : { 8, 16, 24 }) {
        const uint32_t component = (rgba >> shift) & 0xFFu;
        rgba = (rgba & ~(0xFFu << shift)) | (uint32_t (std::lround (component + (255u - component) * 0.42)) << shift);
    }
    return (rgba & 0xFFFFFF00u) | 0xFFu;
}

void SetCell (const layers::Mesh& mesh, Hit& hit)
{
    if (hit.triangle < mesh.hoverCellByTriangle.size ()) {
        const int32_t cell = mesh.hoverCellByTriangle[hit.triangle];
        if (cell >= 0 && size_t (cell) < mesh.hoverCells.size ()) {
            hit.hasCell = true;
            hit.cell = size_t (cell);
            return;
        }
    }
    // Test-built layers may not have passed through the native reader's lookup table.
    for (size_t i = 0; i < mesh.hoverCells.size (); ++i)
        if (std::find (mesh.hoverCells[i].triangles.begin (), mesh.hoverCells[i].triangles.end (), hit.triangle) !=
            mesh.hoverCells[i].triangles.end ()) {
            hit.hasCell = true;
            hit.cell = i;
            return;
        }
}

detail::PickCache s_planPicks, s_viewPicks;

} // namespace

void ClearPickCache ()
{
    s_planPicks = {}; s_viewPicks = {};
}

uint64_t ProjectionKey (const double matrix[16], const float viewport[4])
{
    uint64_t key = 1469598103934665603ull;
    const auto append = [&] (const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*> (data);
        for (size_t i = 0; i < size; ++i) key = (key ^ bytes[i]) * 1099511628211ull;
    };
    append (matrix, sizeof (double) * 16); append (viewport, sizeof (float) * 4);
    return key != 0 ? key : 1;
}

Hit PickPlan (const std::vector<std::shared_ptr<const layers::Layer>>& all, double x, double y)
{
    s_planPicks.Prepare (all, 1, nullptr);
    Hit hit;
    for (size_t li = all.size (); li-- > 0;) {
        const layers::Layer& layer = *all[li];
        if (!layers::DrawnIn (layer.views, layers::Views::TwoD))
            continue;
        for (size_t mi = layer.meshes.size (); mi-- > 0;) {
            const layers::Mesh& mesh = layer.meshes[mi];
            if (!SaysSomething (mesh))
                continue;
            const size_t corners = mesh.points.size () / 3;
            for (const uint32_t triangle : s_planPicks.indices[li][mi].At (x, y)) {
                const size_t t = size_t (triangle) * 3;
                const uint32_t i0 = mesh.indices[t], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
                if (i0 >= corners || i1 >= corners || i2 >= corners)
                    continue;
                double w[3] = {};
                if (!Weights (x, y, &mesh.points[i0 * 3], &mesh.points[i1 * 3], &mesh.points[i2 * 3], w))
                    continue;
                hit.found = true;
                hit.layer = li;
                hit.mesh = mi;
                hit.triangle = uint32_t (t / 3);
                SetCell (mesh, hit);
                if (mesh.values.size () >= corners) {
                    hit.hasValue = true;
                    hit.value = w[0] * mesh.values[i0] + w[1] * mesh.values[i1] + w[2] * mesh.values[i2];
                }
                return hit;
            }
        }
    }
    return hit;
}

overlayhud::Hover Readout (const layers::Layer& layer, const Hit& hit)
{
    overlayhud::Hover hover;
    if (!hit.found || hit.mesh >= layer.meshes.size ())
        return hover;
    const layers::Mesh& mesh = layer.meshes[hit.mesh];
    hover.active = true;
    hover.layer = layer.name;
    hover.title = !mesh.hoverTitle.empty () ? mesh.hoverTitle : layer.name;
    if (hit.hasValue) {
        uint32_t decimals = 2;
        std::string value;
        std::string unit;
        std::string label = "Value";
        if (!layer.legends.empty ()) {
            decimals = layer.legends.front ().decimals;
            unit = layer.legends.front ().unit;
            if (!layer.legends.front ().title.empty ())
                label = layer.legends.front ().title;
        }
        else {
            bool foundRamp = false;
            for (const auto& panel : layer.panels) {
                for (const auto& item : panel.items)
                    if (item.kind == layers::ItemKind::Ramp) {
                        decimals = item.decimals;
                        unit = item.unit;
                        label = item.text.empty () ? panel.title : item.text;
                        foundRamp = true;
                        break;
                    }
                if (foundRamp)
                    break;
            }
        }
        value = Number (hit.value, decimals);
        if (!unit.empty ())
            value += " " + unit;
        hover.rows.emplace_back (label, value);
        hover.measuredValue = hit.value;
        hover.valueRgba = RampColor (mesh, hit.value);
    }
    hover.rows.insert (hover.rows.end (), mesh.hoverRows.begin (), mesh.hoverRows.end ());
    if (hit.hasCell && hit.cell < mesh.hoverCells.size ()) {
        const layers::HoverCell& cell = mesh.hoverCells[hit.cell];
        hover.hasMeasuredCell = true;
        std::copy (std::begin (cell.center), std::end (cell.center), hover.cellCenter);
        std::copy (std::begin (cell.from), std::end (cell.from), hover.measurementFrom);
        std::copy (std::begin (cell.to), std::end (cell.to), hover.measurementTo);
        std::copy (std::begin (cell.normal), std::end (cell.normal), hover.cellNormal);
        hover.rows.emplace_back ("Floor", Number (cell.from[2], 2) + " m");
        hover.rows.emplace_back ("Ceiling", Number (cell.to[2], 2) + " m");
    }
    return hover;
}

bool ProjectThrough (const double m[16], const float viewport[4], double x, double y, double z, float& px, float& py,
                     float& invW)
{
    double clip[4];
    for (int c = 0; c < 4; ++c)
        clip[c] = x * m[c] + y * m[4 + c] + z * m[8 + c] + m[12 + c];
    if (!(clip[3] > 1e-9))
        return false; // behind the eye, or on its plane
    const double ndcX = clip[0] / clip[3], ndcY = clip[1] / clip[3];
    px = float (viewport[0] + (ndcX * 0.5 + 0.5) * viewport[2]);
    py = float (viewport[1] + (0.5 - ndcY * 0.5) * viewport[3]);
    invW = float (1.0 / clip[3]);
    return std::isfinite (px) && std::isfinite (py);
}

Hit PickView (const std::vector<std::shared_ptr<const layers::Layer>>& all, const ProjectView& project, float x,
              float y, uint64_t projectionKey)
{
    Hit hit;
    float nearest = 0.0f; // 1/w of the nearest hit: larger is nearer
    if (projectionKey != 0) s_viewPicks.Prepare (all, projectionKey, &project);
    for (size_t li = 0; li < all.size (); ++li) {
        const layers::Layer& layer = *all[li];
        if (!layers::DrawnIn (layer.views, layers::Views::ThreeD))
            continue;
        for (size_t mi = 0; mi < layer.meshes.size (); ++mi) {
            const layers::Mesh& mesh = layer.meshes[mi];
            if (!SaysSomething (mesh))
                continue;
            const size_t corners = mesh.points.size () / 3;
            detail::PickIndex fresh;
            if (projectionKey == 0) fresh.Build (mesh, &project);
            const auto& index = projectionKey != 0 ? s_viewPicks.indices[li][mi] : fresh;
            const auto& sx = index.sx; const auto& sy = index.sy;
            const auto& iw = index.iw; const auto& seen = index.seen;
            for (const uint32_t triangle : index.At (x, y)) {
                const size_t t = size_t (triangle) * 3;
                const uint32_t i0 = mesh.indices[t], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
                if (i0 >= corners || i1 >= corners || i2 >= corners || !seen[i0] || !seen[i1] || !seen[i2])
                    continue;
                const double a[2] = { sx[i0], sy[i0] }, b[2] = { sx[i1], sy[i1] }, c[2] = { sx[i2], sy[i2] };
                double w[3] = {};
                if (!Weights (x, y, a, b, c, w))
                    continue;
                // 1/w is linear on the screen; the point's own, against the nearest so far.
                const double depth = w[0] * iw[i0] + w[1] * iw[i1] + w[2] * iw[i2];
                if (!(depth > nearest))
                    continue;
                nearest = float (depth);
                hit = Hit ();
                hit.found = true;
                hit.layer = li;
                hit.mesh = mi;
                hit.triangle = uint32_t (t / 3);
                SetCell (mesh, hit);
                if (mesh.values.size () >= corners) {
                    hit.hasValue = true;
                    hit.value = (w[0] * iw[i0] * mesh.values[i0] + w[1] * iw[i1] * mesh.values[i1] +
                                 w[2] * iw[i2] * mesh.values[i2]) /
                                depth;
                }
            }
        }
    }
    return hit;
}

void TintModel (const layers::Mesh& mesh, std::vector<double>& out, int64_t only)
{
    const size_t corners = mesh.points.size () / 3;
    for (size_t t = 0; t + 2 < mesh.indices.size (); t += 3) {
        if (only >= 0 && int64_t (t / 3) != only)
            continue;
        const uint32_t i[3] = { mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2] };
        if (i[0] >= corners || i[1] >= corners || i[2] >= corners)
            continue;
        for (const uint32_t k : i)
            out.insert (out.end (), { mesh.points[k * 3], mesh.points[k * 3 + 1], mesh.points[k * 3 + 2] });
    }
}

void TintModel (const layers::Mesh& mesh, std::vector<double>& out, const std::vector<uint32_t>& onlyTriangles)
{
    const size_t corners = mesh.points.size () / 3;
    for (const uint32_t triangle : onlyTriangles) {
        const size_t t = size_t (triangle) * 3;
        if (t + 2 >= mesh.indices.size ()) continue;
        const uint32_t i[3] = { mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2] };
        if (i[0] >= corners || i[1] >= corners || i[2] >= corners)
            continue;
        for (const uint32_t k : i)
            out.insert (out.end (), { mesh.points[k * 3], mesh.points[k * 3 + 1], mesh.points[k * 3 + 2] });
    }
}

} // namespace overlayhover
} // namespace archviz
} // namespace geomsrv
