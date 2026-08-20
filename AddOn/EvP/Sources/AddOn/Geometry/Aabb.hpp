#ifndef GEOMETRYSERVER_AABB_HPP
#define GEOMETRYSERVER_AABB_HPP

#include <limits>

// Axis-aligned bounding box (world meters). Plain math — no ACAPI/msgpack.
namespace geomsrv {

struct Aabb {
    double mn[3] = { std::numeric_limits<double>::infinity (),
                     std::numeric_limits<double>::infinity (),
                     std::numeric_limits<double>::infinity () };
    double mx[3] = { -std::numeric_limits<double>::infinity (),
                     -std::numeric_limits<double>::infinity (),
                     -std::numeric_limits<double>::infinity () };

    bool Valid () const { return mn[0] <= mx[0]; }

    void Expand (double x, double y, double z)
    {
        if (x < mn[0]) mn[0] = x;  if (x > mx[0]) mx[0] = x;
        if (y < mn[1]) mn[1] = y;  if (y > mx[1]) mx[1] = y;
        if (z < mn[2]) mn[2] = z;  if (z > mx[2]) mx[2] = z;
    }

    bool OverlapsBox (const double bmn[3], const double bmx[3]) const
    {
        return mn[0] <= bmx[0] && mx[0] >= bmn[0] &&
               mn[1] <= bmx[1] && mx[1] >= bmn[1] &&
               mn[2] <= bmx[2] && mx[2] >= bmn[2];
    }

    // Closest-point distance from sphere centre to the box <= radius.
    bool OverlapsSphere (const double c[3], double r) const
    {
        double d2 = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double e = c[i] < mn[i] ? mn[i] - c[i] : (c[i] > mx[i] ? c[i] - mx[i] : 0.0);
            d2 += e * e;
        }
        return d2 <= r * r;
    }
};

} // namespace geomsrv

#endif
