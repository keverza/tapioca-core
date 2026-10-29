#include "ArchViz/AnalysisPath.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::analysispath {

namespace {

constexpr double kArcStepRadians = 3.14159265358979323846 / 36.0; // 5 degrees per chord

double Distance (const Point3& a, const Point3& b)
{
    return std::sqrt ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// The edge from p to q turning through `angle` (counter-clockwise positive), as
// the points after p. Its centre is on the left of p -> q for a positive angle.
void AppendEdge (const Point3& p, const Point3& q, double angle, std::vector<Point3>& out)
{
    const double dx = q[0] - p[0], dy = q[1] - p[1];
    const double chord = std::hypot (dx, dy);
    if (std::abs (angle) < 1.0e-9 || chord < 1.0e-12) {
        out.push_back (q);
        return;
    }
    const double offset = 0.5 * chord / std::tan (0.5 * angle);
    const double cx = 0.5 * (p[0] + q[0]) - dy / chord * offset;
    const double cy = 0.5 * (p[1] + q[1]) + dx / chord * offset;
    const size_t steps = (std::max) (size_t (2), size_t (std::ceil (std::abs (angle) / kArcStepRadians)));
    for (size_t k = 1; k < steps; ++k) {
        const double turn = angle * double (k) / double (steps);
        const double rx = p[0] - cx, ry = p[1] - cy;
        out.push_back ({ cx + rx * std::cos (turn) - ry * std::sin (turn),
                         cy + rx * std::sin (turn) + ry * std::cos (turn), p[2] });
    }
    out.push_back (q); // exactly the next vertex, not the rotation's rounding of it
}

} // namespace

Path Build (std::string guid, const std::vector<double>& xy, const std::vector<double>& arcs, bool closed, double z)
{
    Path path;
    path.guid = std::move (guid);
    path.closed = closed;
    const size_t count = xy.size () / 2;
    if (count < 2)
        return path;
    const auto vertex = [&] (size_t i) { return Point3 { xy[2 * (i % count)], xy[2 * (i % count) + 1], z }; };
    path.points.push_back (vertex (0));
    const size_t edges = closed ? count : count - 1;
    for (size_t i = 0; i < edges; ++i)
        AppendEdge (vertex (i), vertex (i + 1), i < arcs.size () ? arcs[i] : 0.0, path.points);
    path.along.assign (1, 0.0);
    for (size_t i = 1; i < path.points.size (); ++i)
        path.along.push_back (path.along.back () + Distance (path.points[i - 1], path.points[i]));
    if (path.Length () <= 1.0e-9) {
        path.points.clear ();
        path.along.clear ();
    }
    return path;
}

bool Evaluate (const Path& path, double t, Point3& at, Point3& tangent)
{
    if (path.points.size () < 2 || path.along.size () != path.points.size () || path.Length () <= 0.0)
        return false;
    const double target = (std::min) (1.0, (std::max) (0.0, t)) * path.Length ();
    // The first segment ending at or past the target; a zero-length one is skipped.
    size_t i = 1;
    while (i + 1 < path.points.size () && (path.along[i] < target || path.along[i] - path.along[i - 1] <= 1.0e-12))
        ++i;
    const double span = path.along[i] - path.along[i - 1];
    const double f = span > 0.0 ? (std::min) (1.0, (std::max) (0.0, (target - path.along[i - 1]) / span)) : 0.0;
    const Point3& a = path.points[i - 1];
    const Point3& b = path.points[i];
    for (int axis = 0; axis < 3; ++axis) {
        at[axis] = a[axis] + (b[axis] - a[axis]) * f;
        tangent[axis] = span > 0.0 ? (b[axis] - a[axis]) / span : 0.0;
    }
    return span > 0.0;
}

} // namespace geomsrv::archviz::analysispath
