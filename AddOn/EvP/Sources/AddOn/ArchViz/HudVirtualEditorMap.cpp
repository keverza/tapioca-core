#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <cmath>

// OVERLAY-INVARIANTS.md: invert only the supplied host mapping; never acquire or guess a camera.
namespace geomsrv::archviz::edithud::detail {

using edit::Vec3;

namespace {
bool Invertible (double xx, double xy, double yx, double yy)
{
    const double scale = std::hypot (xx, xy) * std::hypot (yx, yy);
    const double det = xx * yy - xy * yx;
    return std::isfinite (scale) && std::isfinite (det) && scale > 1e-12 && std::abs (det) > scale * 1e-6;
}
} // namespace

bool Map::Screen (Vec3 p, ImVec2& out) const
{
    if (!edit::Finite (p))
        return false;
    float x = 0.0f, y = 0.0f;
    if (v.planar) {
        x = float (v.plan[0] * p.x + v.plan[1] * p.y + v.plan[2]);
        y = float (v.plan[3] * p.x + v.plan[4] * p.y + v.plan[5]);
    }
    else if (!v.project || !v.project (p.x, p.y, p.z, x, y))
        return false;
    out = { x, y };
    return std::isfinite (x) && std::isfinite (y);
}

bool Map::Onto (float px, float py, double z, Vec3 seed, Vec3& out) const
{
    return Onto (px, py, edit::WorldXY (z), seed, out);
}

bool Map::Onto (float px, float py, const edit::WorkingPlane& plane, Vec3 seed, Vec3& out) const
{
    if (!edit::ValidPlane (plane) || !edit::Finite (seed) || !std::isfinite (px) || !std::isfinite (py))
        return false;
    if (v.planar) {
        // Invert the supplied affine plan in the plane's authored axes, in double precision.
        const double xx = v.plan[0] * plane.u.x + v.plan[1] * plane.u.y;
        const double xy = v.plan[3] * plane.u.x + v.plan[4] * plane.u.y;
        const double yx = v.plan[0] * plane.v.x + v.plan[1] * plane.v.y;
        const double yy = v.plan[3] * plane.v.x + v.plan[4] * plane.v.y;
        if (!Invertible (xx, xy, yx, yy))
            return false;
        const double det = xx * yy - xy * yx;
        const double dx = px - (v.plan[0] * plane.origin.x + v.plan[1] * plane.origin.y + v.plan[2]);
        const double dy = py - (v.plan[3] * plane.origin.x + v.plan[4] * plane.origin.y + v.plan[5]);
        const Vec3 hit = edit::ToWorld (plane, { (dx * yy - dy * yx) / det, (dy * xx - dx * xy) / det, 0 });
        if (!edit::Finite (hit))
            return false;
        out = hit;
        return true;
    }
    Vec3 local = edit::ToLocal (plane, seed);
    local.z = 0;
    for (int iteration = 0; iteration < 12; ++iteration) {
        const Vec3 at = edit::ToWorld (plane, local);
        ImVec2 p, x, y;
        constexpr double step = 0.1;
        if (!Screen (at, p) || !Screen (at + plane.u * step, x) || !Screen (at + plane.v * step, y))
            return false;
        const double xx = (x.x - p.x) / step, xy = (x.y - p.y) / step, yx = (y.x - p.x) / step, yy = (y.y - p.y) / step;
        // Check rank even at an exact seed hit: a matching pixel on an edge-on plane is ambiguous.
        if (!Invertible (xx, xy, yx, yy))
            return false;
        const double dx = px - p.x, dy = py - p.y;
        if (std::hypot (dx, dy) < 0.05) {
            out = at;
            return true;
        }
        const double det = xx * yy - xy * yx;
        local.x += (dx * yy - dy * yx) / det;
        local.y += (dy * xx - dx * xy) / det;
        if (!edit::Finite (local))
            return false;
    }
    return false;
}

bool Map::RayAt (float px, float py, double z, Vec3 seed, edit::Ray& ray) const
{
    Vec3 low, high;
    if (!Onto (px, py, z, seed, low))
        return false;
    if (v.planar) {
        ray = { low + Vec3 { 0.0, 0.0, 1000.0 }, { 0.0, 0.0, -1.0 } };
        return true;
    }
    if (!Onto (px, py, z + 1.0, low, high))
        return false;
    Vec3 direction = low - high;
    if (v.hasCameraDirection &&
        Dot (direction, Vec3 { v.cameraDirection[0], v.cameraDirection[1], v.cameraDirection[2] }) < 0.0)
        direction = direction * -1.0;
    if (!Normalize (direction, direction))
        return false;
    ray = { low - direction * 1000.0, direction };
    return true;
}

double Map::MetresPerPixel (float px, float py, double z, Vec3 seed) const
{
    double scale = 0.01;
    MetresPerPixel (px, py, edit::WorldXY (z), seed, scale);
    return scale;
}

bool Map::MetresPerPixel (float px, float py, const edit::WorkingPlane& plane, Vec3 seed, double& out) const
{
    Vec3 a, b;
    if (!Onto (px, py, plane, seed, a) || !Onto (px + 1.0f, py, plane, a, b))
        return false;
    const double d = Distance (a, b);
    if (d <= 1.0e-9 || !std::isfinite (d))
        return false;
    out = d;
    return true;
}

edit::PickView Map::Pick () const
{
    edit::PickView view;
    view.toScreen = [this] (Vec3 p, double& x, double& y) {
        ImVec2 s;
        if (!Screen (p, s))
            return false;
        x = s.x;
        y = s.y;
        return true;
    };
    if (!v.planar && v.hasCameraDirection)
        view.viewDirection = { v.cameraDirection[0], v.cameraDirection[1], v.cameraDirection[2] };
    return view;
}

} // namespace geomsrv::archviz::edithud::detail
