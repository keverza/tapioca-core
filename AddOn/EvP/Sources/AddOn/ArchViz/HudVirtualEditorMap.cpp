#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <cmath>

// OVERLAY-INVARIANTS.md: invert only the supplied host mapping; never acquire or guess a camera.
namespace geomsrv::archviz::edithud::detail {

using edit::Vec3;

bool Map::Screen (Vec3 p, ImVec2& out) const
{
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
    if (v.planar) {
        const double det = v.plan[0] * v.plan[4] - v.plan[1] * v.plan[3];
        if (std::abs (det) < 1.0e-12)
            return false;
        const double dx = px - v.plan[2], dy = py - v.plan[5];
        out = { (dx * v.plan[4] - v.plan[1] * dy) / det, (v.plan[0] * dy - v.plan[3] * dx) / det, z };
        return true;
    }
    Vec3 at { seed.x, seed.y, z };
    for (int iteration = 0; iteration < 12; ++iteration) {
        ImVec2 p, x, y;
        constexpr double step = 0.1;
        if (!Screen (at, p) || !Screen (at + Vec3 { step, 0.0, 0.0 }, x) || !Screen (at + Vec3 { 0.0, step, 0.0 }, y))
            return false;
        const double dx = px - p.x, dy = py - p.y;
        if (std::hypot (dx, dy) < 0.05) {
            out = at;
            return true;
        }
        const double xx = (x.x - p.x) / step, xy = (x.y - p.y) / step, yx = (y.x - p.x) / step, yy = (y.y - p.y) / step;
        const double det = xx * yy - xy * yx;
        if (std::abs (det) < 1.0e-6)
            return false;
        at.x += (dx * yy - dy * yx) / det;
        at.y += (dy * xx - dx * xy) / det;
        if (!Finite (at))
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
    Vec3 a, b;
    if (!Onto (px, py, z, seed, a) || !Onto (px + 1.0f, py, z, a, b))
        return 0.01;
    const double d = Distance (a, b);
    return d > 1.0e-9 && std::isfinite (d) ? d : 0.01;
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
