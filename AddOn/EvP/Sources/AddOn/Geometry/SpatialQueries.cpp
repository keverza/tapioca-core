#include "SpatialQueries.hpp"

namespace geomsrv {

namespace {

// Point-in-polygon (even-odd ray cast) in XY.
bool PointInPoly (double px, double py, const std::vector<double>& p)
{
    const size_t n = p.size () / 2;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = p[i*2], yi = p[i*2+1], xj = p[j*2], yj = p[j*2+1];
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

// Do segments (a,b) and (c,d) intersect (proper or touching)?
bool SegIntersect (double ax, double ay, double bx, double by,
                   double cx, double cy, double dx, double dy)
{
    auto cross = [] (double ox, double oy, double px, double py, double qx, double qy) {
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox);
    };
    const double d1 = cross (cx, cy, dx, dy, ax, ay);
    const double d2 = cross (cx, cy, dx, dy, bx, by);
    const double d3 = cross (ax, ay, bx, by, cx, cy);
    const double d4 = cross (ax, ay, bx, by, dx, dy);
    if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)))
        return true;
    return false;
}

// XY rectangle [rmnx,rmny]-[rmxx,rmxy] vs polygon: overlap if a rect corner is
// inside the polygon, a polygon vertex is inside the rect, or any edges cross.
bool RectPolyIntersect (double rmnx, double rmny, double rmxx, double rmxy,
                        const std::vector<double>& poly)
{
    if (PointInPoly (rmnx, rmny, poly) || PointInPoly (rmxx, rmny, poly) ||
        PointInPoly (rmxx, rmxy, poly) || PointInPoly (rmnx, rmxy, poly))
        return true;

    const size_t n = poly.size () / 2;
    const double rx[5] = { rmnx, rmxx, rmxx, rmnx, rmnx };
    const double ry[5] = { rmny, rmny, rmxy, rmxy, rmny };
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double px = poly[i*2], py = poly[i*2+1];
        if (px >= rmnx && px <= rmxx && py >= rmny && py <= rmxy)
            return true;   // polygon vertex inside rect
        const double qx = poly[j*2], qy = poly[j*2+1];
        for (int k = 0; k < 4; ++k)
            if (SegIntersect (px, py, qx, qy, rx[k], ry[k], rx[k+1], ry[k+1]))
                return true;
    }
    return false;
}

} // namespace

std::vector<std::string> QueryBox (const Snapshot& snap, const double mn[3], const double mx[3])
{
    std::vector<std::string> out;
    for (const auto& m : snap.meshes)
        if (m.bounds.Valid () && m.bounds.OverlapsBox (mn, mx))
            out.push_back (m.guid);
    return out;
}

std::vector<std::string> QuerySphere (const Snapshot& snap, const double c[3], double radius)
{
    std::vector<std::string> out;
    for (const auto& m : snap.meshes)
        if (m.bounds.Valid () && m.bounds.OverlapsSphere (c, radius))
            out.push_back (m.guid);
    return out;
}

std::vector<std::string> QueryPolygon (const Snapshot& snap, const std::vector<double>& polyXY,
                                       double zmin, double zmax)
{
    std::vector<std::string> out;
    if (polyXY.size () < 6)   // need >= 3 points
        return out;
    for (const auto& m : snap.meshes) {
        const Aabb& b = m.bounds;
        if (!b.Valid ())
            continue;
        if (b.mx[2] < zmin || b.mn[2] > zmax)   // Z ranges disjoint
            continue;
        if (RectPolyIntersect (b.mn[0], b.mn[1], b.mx[0], b.mx[1], polyXY))
            out.push_back (m.guid);
    }
    return out;
}

} // namespace geomsrv
