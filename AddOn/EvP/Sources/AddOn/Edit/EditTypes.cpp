#include "Edit/EditTypes.hpp"
#include <cmath>

namespace geomsrv::edit {

Vec3 operator+ (Vec3 a, Vec3 b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3 operator- (Vec3 a, Vec3 b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

Vec3 operator* (Vec3 a, double s)
{
    return { a.x * s, a.y * s, a.z * s };
}

bool operator== (Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

double Dot (Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross (Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

double Length (Vec3 a)
{
    return std::sqrt (Dot (a, a));
}

double Distance (Vec3 a, Vec3 b)
{
    return Length (a - b);
}

double DistanceXY (Vec3 a, Vec3 b)
{
    return std::hypot (a.x - b.x, a.y - b.y);
}

Vec3 Lerp (Vec3 a, Vec3 b, double t)
{
    return a + (b - a) * t;
}

bool Finite (Vec3 a)
{
    return std::isfinite (a.x) && std::isfinite (a.y) && std::isfinite (a.z);
}

bool Normalize (Vec3 a, Vec3& out)
{
    const double length = Length (a);
    if (!std::isfinite (length) || length < 1.0e-12)
        return false;
    out = a * (1.0 / length);
    return true;
}

} // namespace geomsrv::edit
