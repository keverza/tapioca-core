#include "SunStudy/SunStudySampler.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {
namespace {

Vec3 Sub (const Vec3& a, const Vec3& b)
{
    return Vec3 { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

double Dot (const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return Vec3 { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

} // namespace

Vec3 TriangleNormal (const Vec3& a, const Vec3& b, const Vec3& c, double* areaOut)
{
    const Vec3 n = Cross (Sub (b, a), Sub (c, a));
    const double length = std::sqrt (Dot (n, n));
    if (areaOut != nullptr)
        *areaOut = 0.5 * length;
    if (!(length > 0.0))
        return Vec3 { 0.0, 0.0, 0.0 };
    return Vec3 { n[0] / length, n[1] / length, n[2] / length };
}

} // namespace evp::sunstudy
