#ifndef GEOMETRYSERVER_TRIANGLEDEGENERACY_HPP
#define GEOMETRYSERVER_TRIANGLEDEGENERACY_HPP

#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv {
// Source convex contours can contain collinear corners or zero-area decomposition
// pieces. Do not emit their empty FAN primitives as solid boundary triangles.
// This is a floating-point roundoff test, not a contact/area tolerance: small but
// nonzero triangles remain. Invalid numbers are deliberately NOT classified empty.
inline bool IsRoundoffZeroTriangle (const double* a, const double* b, const double* c)
{
    double u[3], v[3];
    for (size_t i = 0; i < 3; ++i) {
        if (!std::isfinite (a[i]) || !std::isfinite (b[i]) || !std::isfinite (c[i]))
            return false;
        u[i] = b[i] - a[i];
        v[i] = c[i] - a[i];
    }
    const double scale = std::max (
        { std::abs (u[0]), std::abs (u[1]), std::abs (u[2]), std::abs (v[0]), std::abs (v[1]), std::abs (v[2]) });
    if (!std::isfinite (scale))
        return false;
    if (scale == 0)
        return true;
    for (size_t i = 0; i < 3; ++i) {
        u[i] /= scale;
        v[i] /= scale;
    }
    const double cross = std::hypot (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]);
    return cross <= 8 * std::numeric_limits<double>::epsilon ();
}
} // namespace geomsrv
#endif
