#include "Edit/EditPath.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::edit {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStraight = 1.0e-9;    // deflection below this: no corner to round
constexpr double kDoublesBack = 1.0e-6; // deflection this close to pi: refused

Vec3 Flat (Vec3 p)
{
    return { p.x, p.y, 0.0 };
}

Vec3 Left (Vec3 d)
{
    return { -d.y, d.x, 0.0 };
}

double Cross2 (Vec3 a, Vec3 b)
{
    return a.x * b.y - a.y * b.x;
}

} // namespace

bool BuildAlignment (const std::vector<ControlPoint>& points, AlignmentGeometry& out, std::string& error)
{
    out = {};
    const size_t n = points.size ();
    if (n < 2) {
        error = "a path needs two control points";
        return false;
    }
    std::vector<Vec3> dir (n - 1);
    std::vector<double> leg (n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        if (!Finite (points[i].p) || !Finite (points[i + 1].p)) {
            error = "a control point is not finite";
            return false;
        }
        leg[i] = DistanceXY (points[i].p, points[i + 1].p);
        if (leg[i] < 1.0e-6 || !Normalize (Flat (points[i + 1].p - points[i].p), dir[i])) {
            error = "two control points coincide in plan";
            return false;
        }
    }
    // Tangent length and turn of every interior corner.
    std::vector<double> tangent (n, 0.0), deflection (n, 0.0), turn (n, 0.0);
    for (size_t i = 1; i + 1 < n; ++i) {
        const double c = std::clamp (Dot (dir[i - 1], dir[i]), -1.0, 1.0);
        deflection[i] = std::acos (c);
        turn[i] = Cross2 (dir[i - 1], dir[i]) >= 0.0 ? 1.0 : -1.0;
        if (deflection[i] > kPi - kDoublesBack) {
            error = "the path doubles back at control point " + std::to_string (i + 1);
            return false;
        }
        const double r = points[i].radius;
        if (!(r >= 0.0) || !std::isfinite (r)) {
            error = "a corner radius cannot be negative";
            return false;
        }
        if (r > 0.0 && deflection[i] > kStraight)
            tangent[i] = r * std::tan (deflection[i] * 0.5);
    }
    for (size_t i = 0; i + 1 < n; ++i)
        if (tangent[i] + tangent[i + 1] > leg[i] + 1.0e-9) {
            error = "the corner radius at control point " +
                    std::to_string (tangent[i + 1] > tangent[i] ? i + 2 : i + 1) + " does not fit its legs";
            return false;
        }

    double station = 0.0;
    Vec3 cursor = Flat (points[0].p);
    out.pointStations.assign (n, 0.0);
    const auto line = [&] (Vec3 to) {
        const double length = DistanceXY (cursor, to);
        if (length > 1.0e-12) {
            AlignmentElement e;
            e.kind = ElementKind::Line;
            e.start = station;
            e.length = length;
            e.a = cursor;
            e.b = to;
            out.elements.push_back (e);
            station += length;
        }
        cursor = to;
    };
    for (size_t i = 1; i + 1 < n; ++i) {
        const Vec3 p = Flat (points[i].p);
        if (tangent[i] <= 0.0) {
            line (p);
            out.pointStations[i] = station;
            continue;
        }
        const Vec3 tc = p - dir[i - 1] * tangent[i], ct = p + dir[i] * tangent[i];
        line (tc);
        AlignmentElement arc;
        arc.kind = ElementKind::Arc;
        arc.start = station;
        arc.radius = points[i].radius;
        arc.length = arc.radius * deflection[i];
        arc.centre = tc + Left (dir[i - 1]) * (arc.radius * turn[i]);
        arc.startAngle = std::atan2 (tc.y - arc.centre.y, tc.x - arc.centre.x);
        arc.sweep = turn[i] * deflection[i];
        arc.a = tc;
        arc.b = ct;
        out.elements.push_back (arc);
        out.pointStations[i] = station + arc.length * 0.5;
        station += arc.length;
        cursor = ct;
    }
    line (Flat (points[n - 1].p));
    out.length = station;
    out.pointStations[n - 1] = station;
    return true;
}

static void Place (const AlignmentElement& e, double along, Placement& out)
{
    if (e.kind == ElementKind::Line) {
        Normalize (e.b - e.a, out.tangent);
        out.point = Lerp (e.a, e.b, e.length > 0.0 ? along / e.length : 0.0);
        out.curvature = 0.0;
    }
    else {
        const double sign = e.sweep >= 0.0 ? 1.0 : -1.0;
        const double angle = e.startAngle + sign * along / e.radius;
        out.point = { e.centre.x + e.radius * std::cos (angle), e.centre.y + e.radius * std::sin (angle), 0.0 };
        out.tangent = Vec3 { -std::sin (angle), std::cos (angle), 0.0 } * sign;
        out.curvature = sign / e.radius;
    }
    out.left = Left (out.tangent);
}

bool Locate (const AlignmentGeometry& g, double station, Placement& out)
{
    if (g.elements.empty () || !(station >= -1.0e-9) || !(station <= g.length + 1.0e-9))
        return false;
    station = std::clamp (station, 0.0, g.length);
    for (const auto& e : g.elements)
        if (station <= e.start + e.length || &e == &g.elements.back ()) {
            Place (e, std::clamp (station - e.start, 0.0, e.length), out);
            return true;
        }
    return false;
}

bool StationOf (const AlignmentGeometry& g, Vec3 point, double& station, double& offset)
{
    double best = std::numeric_limits<double>::infinity ();
    const Vec3 p = Flat (point);
    for (const auto& e : g.elements) {
        double along = 0.0;
        if (e.kind == ElementKind::Line) {
            Vec3 d;
            Normalize (e.b - e.a, d);
            along = std::clamp (Dot (p - e.a, d), 0.0, e.length);
        }
        else {
            const double sign = e.sweep >= 0.0 ? 1.0 : -1.0;
            double angle = std::atan2 (p.y - e.centre.y, p.x - e.centre.x) - e.startAngle;
            angle = std::remainder (angle * sign, 2.0 * kPi);
            // Outside the arc's span, the nearer end wins.
            const double span = std::abs (e.sweep);
            if (angle < 0.0 || angle > span) {
                const double toStart = std::abs (std::remainder (angle, 2.0 * kPi));
                const double toEnd = std::abs (std::remainder (angle - span, 2.0 * kPi));
                angle = toStart <= toEnd ? 0.0 : span;
            }
            along = angle * e.radius;
        }
        Placement at;
        Place (e, along, at);
        const double d = DistanceXY (at.point, p);
        if (d < best) {
            best = d;
            station = e.start + along;
            offset = Dot (p - at.point, at.left);
        }
    }
    return std::isfinite (best);
}

std::vector<double> ElementStations (const AlignmentGeometry& g)
{
    std::vector<double> out;
    for (const auto& e : g.elements)
        out.push_back (e.start);
    out.push_back (g.length);
    return out;
}

} // namespace geomsrv::edit
