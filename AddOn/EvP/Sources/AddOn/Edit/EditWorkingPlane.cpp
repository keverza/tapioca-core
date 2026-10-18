#include "Edit/EditWorkingPlane.hpp"
#include "Edit/EditGizmo.hpp"
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
        (p.source != PlaneSource::WorldXY && p.source != PlaneSource::Explicit && p.source != PlaneSource::Face &&
         p.source != PlaneSource::PathPlan && p.source != PlaneSource::PathSection) ||
        (p.source == PlaneSource::Face && (p.sourceRef.entity == kNoId || p.sourceRef.part != Part::Face)) ||
        !std::isfinite (p.sourceStation) || p.sourceStation < 0 ||
        ((p.source == PlaneSource::PathPlan || p.source == PlaneSource::PathSection) &&
         (p.sourceRef.entity == kNoId || p.sourceRef.part != Part::Entity)))
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
    return a.origin == b.origin && a.u == b.u && a.v == b.v && a.normal == b.normal && a.source == b.source &&
           a.sourceRef == b.sourceRef && a.sourceStation == b.sourceStation;
}

bool SamePlane (const WorkingPlane& a, const WorkingPlane& b)
{
    return SamePlaneFrame (a, b) && a.locked == b.locked && a.sticky == b.sticky &&
           a.sourceRevision == b.sourceRevision;
}

Plane SnapPlane (const WorkingPlane& p)
{
    return { p.origin, p.normal, p.u };
}

bool PlaneFromFace (const Document& doc, Ref face, WorkingPlane& out, std::string& error)
{
    const Entity* entity = Find (doc, face.entity);
    Frame frame;
    if (face.part != Part::Face || entity == nullptr || entity->vertices.size () < 3 ||
        face.item >= FaceCount (*entity) || !FaceFrame (*entity, size_t (face.item), frame)) {
        error = "Pick one existing virtual extrusion face to set the working plane.";
        return false;
    }
    WorkingPlane plane;
    plane.origin = frame.origin;
    plane.u = frame.axes[0], plane.v = frame.axes[1], plane.normal = frame.axes[2];
    plane.source = PlaneSource::Face;
    plane.sourceRef = face;
    plane.sourceRevision = doc.revision;
    if (!ValidPlane (plane)) {
        error = "That face does not define a finite working plane.";
        return false;
    }
    const auto polygon = FacePolygon (*entity, size_t (face.item));
    Vec3 area;
    for (size_t i = 0; i < polygon.size (); ++i) {
        const auto point = polygon[i];
        if (!Finite (point) || std::abs (Dot (point - plane.origin, plane.normal)) > 1e-8) {
            error = "That face is not planar; the working plane was left unchanged.";
            return false;
        }
        area = area + Cross (point - plane.origin, polygon[(i + 1) % polygon.size ()] - plane.origin);
    }
    if (!Finite (area) || std::abs (Dot (area, plane.normal)) < 1e-12) {
        error = "That face is degenerate; the working plane was left unchanged.";
        return false;
    }
    out = plane;
    error.clear ();
    return true;
}

bool PlaneFromPath (const Document& doc, Id id, double station, PathPlaneKind kind, WorkingPlane& out,
                    std::string& error)
{
    const Path* path = FindPath (doc, id);
    if (path == nullptr || !std::isfinite (station) ||
        (kind != PathPlaneKind::Plan && kind != PathPlaneKind::Section)) {
        error = "Choose an existing virtual path and a finite station in metres from its start.";
        return false;
    }
    AlignmentGeometry alignment;
    if (!ValidatePath (*path, alignment, error))
        return false;
    if (station < 0 || station > alignment.length) {
        error = "The working-plane station must be within the path; it was not clamped to an end.";
        return false;
    }
    for (size_t i = 1; i + 1 < path->points.size (); ++i) {
        if (path->points[i].radius != 0 || std::abs (station - alignment.pointStations[i]) > 1e-8)
            continue;
        Vec3 incoming, outgoing;
        const auto a = path->points[i].p - path->points[i - 1].p;
        const auto b = path->points[i + 1].p - path->points[i].p;
        if (Normalize ({ a.x, a.y, 0 }, incoming) && Normalize ({ b.x, b.y, 0 }, outgoing) &&
            Dot (incoming, outgoing) < 1 - 1e-9) {
            error = "A sharp corner has two tangents; choose a station before or after it.";
            return false;
        }
    }
    StationFrame frame;
    if (!Evaluate (*path, alignment, station, frame)) {
        error = "The path cannot be evaluated at that working-plane station.";
        return false;
    }
    WorkingPlane plane;
    plane.origin = frame.centre;
    if (kind == PathPlaneKind::Plan) {
        plane.u = frame.tangent, plane.v = frame.left;
        plane.source = PlaneSource::PathPlan;
    }
    else {
        plane.u = frame.left, plane.v = { 0, 0, 1 }, plane.normal = frame.tangent;
        plane.source = PlaneSource::PathSection;
    }
    plane.sourceRef = { id, Part::Entity, 0 };
    plane.sourceRevision = doc.revision;
    plane.sourceStation = station;
    if (!ValidPlane (plane)) {
        error = "The path does not define a finite orthonormal station frame.";
        return false;
    }
    out = plane;
    error.clear ();
    return true;
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
