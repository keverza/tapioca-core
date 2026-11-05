#include "Edit/EditPolylineDetail.hpp"
#include <cmath>

namespace geomsrv::edit::polyline_detail {

// Plan crossing of lines a + t (b - a) and c + s (d - c).
bool CrossPlan (Vec3 a, Vec3 b, Vec3 c, Vec3 d, double& t, double& s)
{
    const double rx = b.x - a.x, ry = b.y - a.y, qx = d.x - c.x, qy = d.y - c.y;
    const double den = rx * qy - ry * qx;
    const double scale = std::hypot (rx, ry) * std::hypot (qx, qy);
    if (!(scale > 0.0) || std::abs (den) < 1.0e-9 * scale)
        return false;
    t = ((c.x - a.x) * qy - (c.y - a.y) * qx) / den;
    s = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
    return true;
}

// All edges of `target`, in plan.
std::vector<std::pair<Vec3, Vec3>> EdgesOf (const Entity& target)
{
    std::vector<std::pair<Vec3, Vec3>> out;
    for (size_t i = 0; i < SegmentCount (target); ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (target, i, from, to);
        out.push_back ({ target.vertices[from].p, target.vertices[to].p });
    }
    return out;
}

} // namespace geomsrv::edit::polyline_detail
