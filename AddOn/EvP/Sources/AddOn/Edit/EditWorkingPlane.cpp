#include "Edit/EditWorkingPlane.hpp"
#include <cmath>

namespace geomsrv::edit {

WorkingPlane WorldXY (double z)
{
    WorkingPlane p;
    p.origin.z = z;
    return p;
}

bool ValidPlane (const WorkingPlane& p)
{
    if (!Finite (p.origin) || !Finite (p.u) || !Finite (p.v) || !Finite (p.normal) ||
        (p.source != PlaneSource::WorldXY && p.source != PlaneSource::Explicit))
        return false;
    constexpr double e = 1e-9;
    return std::abs (Length (p.u) - 1) < e && std::abs (Length (p.v) - 1) < e && std::abs (Length (p.normal) - 1) < e &&
           std::abs (Dot (p.u, p.v)) < e && Distance (Cross (p.u, p.v), p.normal) < e;
}

bool IsWorldXY (const WorkingPlane& p)
{
    return ValidPlane (p) && p.source == PlaneSource::WorldXY && p.u == Vec3 { 1, 0, 0 } && p.v == Vec3 { 0, 1, 0 } &&
           p.normal == Vec3 { 0, 0, 1 };
}

bool SamePlaneFrame (const WorkingPlane& a, const WorkingPlane& b)
{
    return a.origin == b.origin && a.u == b.u && a.v == b.v && a.normal == b.normal && a.source == b.source;
}

bool SamePlane (const WorkingPlane& a, const WorkingPlane& b)
{
    return SamePlaneFrame (a, b) && a.locked == b.locked && a.sticky == b.sticky;
}

Plane SnapPlane (const WorkingPlane& p)
{
    return { p.origin, p.normal };
}

Vec3 ToLocal (const WorkingPlane& p, Vec3 world)
{
    const Vec3 d = world - p.origin;
    return { Dot (d, p.u), Dot (d, p.v), Dot (d, p.normal) };
}

Vec3 ToWorld (const WorkingPlane& p, Vec3 local)
{
    return p.origin + p.u * local.x + p.v * local.y + p.normal * local.z;
}

bool HitPlane (const WorkingPlane& p, Vec3 origin, Vec3 direction, Vec3& out)
{
    Vec3 dir;
    if (!ValidPlane (p) || !Finite (origin) || !Normalize (direction, dir))
        return false;
    const double denom = Dot (dir, p.normal);
    if (std::abs (denom) < 1e-9)
        return false;
    const double t = Dot (p.origin - origin, p.normal) / denom;
    const Vec3 hit = origin + dir * t;
    if (t < 0 || !std::isfinite (t) || !Finite (hit))
        return false;
    out = hit;
    return true;
}

bool ChangePlane (WorkingPlane& plane, const WorkingPlane& next, bool automatic, std::string& error)
{
    if (!ValidPlane (next)) {
        error = "The working plane must have a finite origin and right-handed orthonormal axes.";
        return false;
    }
    if (automatic && plane.locked && !SamePlane (plane, next)) {
        error = "The working plane is locked; automatic plane changes are off.";
        return false;
    }
    plane = next;
    error.clear ();
    return true;
}

} // namespace geomsrv::edit
