// ArchViz/OverlayHover -- see the header.

#include "ArchViz/OverlayHover.hpp"

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
    return !mesh.hoverTitle.empty () || !mesh.hoverRows.empty () || !mesh.values.empty ();
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

} // namespace

Hit PickPlan (const std::vector<std::shared_ptr<const layers::Layer>>& all, double x, double y)
{
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
            for (size_t t = 0; t + 2 < mesh.indices.size (); t += 3) {
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
    hover.title = !mesh.hoverTitle.empty () ? mesh.hoverTitle : layer.name;
    if (hit.hasValue) {
        uint32_t decimals = 2;
        std::string value;
        std::string unit;
        if (!layer.legends.empty ()) {
            decimals = layer.legends.front ().decimals;
            unit = layer.legends.front ().unit;
        }
        value = Number (hit.value, decimals);
        if (!unit.empty ())
            value += " " + unit;
        const std::string label = !layer.legends.empty () && !layer.legends.front ().title.empty ()
                                      ? layer.legends.front ().title
                                      : std::string ("Value");
        hover.rows.emplace_back (label, value);
    }
    hover.rows.insert (hover.rows.end (), mesh.hoverRows.begin (), mesh.hoverRows.end ());
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
              float y)
{
    Hit hit;
    float nearest = 0.0f; // 1/w of the nearest hit: larger is nearer
    std::vector<float> sx, sy, iw;
    std::vector<uint8_t> seen;
    for (size_t li = 0; li < all.size (); ++li) {
        const layers::Layer& layer = *all[li];
        if (!layers::DrawnIn (layer.views, layers::Views::ThreeD))
            continue;
        for (size_t mi = 0; mi < layer.meshes.size (); ++mi) {
            const layers::Mesh& mesh = layer.meshes[mi];
            if (!SaysSomething (mesh))
                continue;
            // Each corner projected once.
            const size_t corners = mesh.points.size () / 3;
            sx.assign (corners, 0.0f);
            sy.assign (corners, 0.0f);
            iw.assign (corners, 0.0f);
            seen.assign (corners, 0);
            for (size_t k = 0; k < corners; ++k)
                seen[k] =
                    project (mesh.points[k * 3], mesh.points[k * 3 + 1], mesh.points[k * 3 + 2], sx[k], sy[k], iw[k])
                        ? 1
                        : 0;
            for (size_t t = 0; t + 2 < mesh.indices.size (); t += 3) {
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

} // namespace overlayhover
} // namespace archviz
} // namespace geomsrv
