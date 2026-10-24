#include "Edit/EditImport.hpp"
#include <cmath>

namespace geomsrv::edit {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kQuarter = kPi * 0.5;
constexpr double kMinimum = 1.0e-9;

struct Run {
    std::vector<Vec3> points, in, out;
    std::vector<bool> smooth;
    void Add (Vec3 p, bool interior)
    {
        points.push_back (p);
        in.push_back ({});
        out.push_back ({});
        smooth.push_back (interior);
    }
};

// The arc from `a` to `b` turning `angle`, after `a` (already in the run): its pieces' ends and
// handles, a quarter turn at most each. The centre is left of the chord for a counter-clockwise
// arc under half a turn, right of it over half a turn -- (L/2) / tan(angle / 2) along the left normal.
bool AppendArc (Run& run, Vec3 a, Vec3 b, double angle, bool last)
{
    const double dx = b.x - a.x, dy = b.y - a.y, chord = std::hypot (dx, dy);
    if (chord < kMinimum || std::abs (angle) < kMinimum || std::abs (angle) >= 2.0 * kPi)
        return false;
    const double t = std::tan (angle * 0.5);
    const double cx = (a.x + b.x) * 0.5 - dy / chord * (chord * 0.5 / t);
    const double cy = (a.y + b.y) * 0.5 + dx / chord * (chord * 0.5 / t);
    const double radius = std::hypot (a.x - cx, a.y - cy), start = std::atan2 (a.y - cy, a.x - cx);
    const int pieces = std::max (1, int (std::ceil (std::abs (angle) / kQuarter - 1.0e-9)));
    const double step = angle / pieces, handle = 4.0 / 3.0 * std::tan (step * 0.25) * radius;
    const auto tangent = [] (double at) { return Vec3 { -std::sin (at), std::cos (at), 0.0 }; };
    for (int k = 0; k < pieces; ++k) {
        const double from = start + step * k, to = start + step * (k + 1);
        run.out.back () = tangent (from) * handle;
        const double f = double (k + 1) / pieces;
        const Vec3 end = k + 1 == pieces
                             ? b
                             : Vec3 { cx + radius * std::cos (to), cy + radius * std::sin (to), a.z + (b.z - a.z) * f };
        if (k + 1 == pieces && last) { // a closing arc ends on the run's first point: its handle too
            run.in.front () = tangent (to) * -handle;
            break;
        }
        run.Add (end, k + 1 < pieces); // the arc's own subdivisions are smooth
        run.in.back () = tangent (to) * -handle;
    }
    return true;
}

Id Publish (Document& doc, Run& run, bool closed, const std::string& role, std::string& error)
{
    const Id id = AddPolyline (doc, run.points, closed, role);
    Entity* e = Find (doc, id);
    if (e == nullptr || e->vertices.size () != run.points.size ()) {
        error = "The imported line could not be added.";
        return kNoId;
    }
    for (size_t i = 0; i < run.points.size (); ++i) {
        e->vertices[i].in = run.in[i];
        e->vertices[i].out = run.out[i];
        e->vertices[i].smooth = run.smooth[i];
    }
    return id;
}
} // namespace

Id AddImportedCurve (Document& doc, const ImportedCurve& curve, const std::string& role, std::string& error)
{
    auto points = curve.points;
    // A closed outline given with its first point repeated at the end: once.
    if (curve.closed && points.size () > 2 && Distance (points.front (), points.back ()) < kMinimum)
        points.pop_back ();
    const size_t segments = curve.closed ? points.size () : points.size () - (points.empty () ? 0 : 1);
    if (points.size () < 2) {
        error = "A line needs two points.";
        return kNoId;
    }
    for (const auto& p : points)
        if (!Finite (p)) {
            error = "A point of the line is not finite.";
            return kNoId;
        }
    Run run;
    run.Add (points[0], false);
    for (size_t i = 0; i < segments; ++i) {
        const Vec3 a = points[i], b = points[(i + 1) % points.size ()];
        const double angle = i < curve.arcs.size () ? curve.arcs[i] : 0.0;
        const bool last = curve.closed && i + 1 == segments; // it ends on the run's first point
        if (std::abs (angle) > kMinimum) {
            if (!std::isfinite (angle) || !AppendArc (run, a, b, angle, last)) {
                error = "An arc of the line has no chord or an angle of a whole turn.";
                return kNoId;
            }
        }
        else if (!last)
            run.Add (b, false);
    }
    if (run.points.size () < 2) {
        error = "A line needs two distinct points.";
        return kNoId;
    }
    return Publish (doc, run, curve.closed, role, error);
}

Id AddImportedEllipse (Document& doc, const ImportedEllipse& ellipse, const std::string& role, std::string& error)
{
    if (!Finite (ellipse.centre) || !(ellipse.radius > kMinimum) || !(ellipse.ratio > kMinimum) ||
        !std::isfinite (ellipse.axisAngle) || !std::isfinite (ellipse.from) || !std::isfinite (ellipse.to)) {
        error = "An arc or circle needs a finite centre and positive radii.";
        return kNoId;
    }
    double span = 2.0 * kPi;
    if (!ellipse.whole) {
        span = std::fmod (ellipse.to - ellipse.from, 2.0 * kPi);
        if (span <= kMinimum)
            span += 2.0 * kPi;
    }
    const int pieces = std::max (1, int (std::ceil (span / kQuarter - 1.0e-9)));
    const double step = span / pieces, handle = 4.0 / 3.0 * std::tan (step * 0.25);
    const double c = std::cos (ellipse.axisAngle), s = std::sin (ellipse.axisAngle);
    // The unit circle scaled to the axes and turned to the main axis: points and handles alike.
    const auto map = [&] (double x, double y) {
        const double u = ellipse.radius * x, v = ellipse.radius * ellipse.ratio * y;
        return Vec3 { u * c - v * s, u * s + v * c, 0.0 };
    };
    Run run;
    const int count = ellipse.whole ? pieces : pieces + 1;
    for (int k = 0; k < count; ++k) {
        const double at = ellipse.from + step * k;
        run.Add (ellipse.centre + map (std::cos (at), std::sin (at)), ellipse.whole || (k > 0 && k + 1 < count));
        const Vec3 tangent = map (-std::sin (at), std::cos (at));
        if (ellipse.whole || k + 1 < count)
            run.out.back () = tangent * handle;
        if (ellipse.whole || k > 0)
            run.in.back () = tangent * -handle;
    }
    return Publish (doc, run, ellipse.whole, role, error);
}

} // namespace geomsrv::edit
