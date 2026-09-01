#include "Geometry/Curves.hpp"

#include <cmath>

namespace geomsrv::engine {
namespace {

Vector3 Subtract (const Vector3& left, const Vector3& right)
{
    return { left.x - right.x, left.y - right.y, left.z - right.z };
}

Vector3 Scale (const Vector3& value, double by)
{
    return { value.x * by, value.y * by, value.z * by };
}

double Length (const Vector3& value)
{
    return std::sqrt (value.x * value.x + value.y * value.y + value.z * value.z);
}

// Any unit vector perpendicular to `normal`.
//
// ⚠️ THE SEED AXIS IS CHOSEN AWAY FROM THE NORMAL, NOT FIXED. Crossing with a
// constant axis produces a zero-length result whenever the normal happens to be
// that axis - which for a Z-up model is the single commonest case there is, a
// horizontal arc. The result then has no direction and the arc collapses to a
// point at its centre.
bool PerpendicularTo (const Vector3& normal, Vector3& result, std::string& error)
{
    const Vector3 seed = std::fabs (normal.z) < 0.9 ? Vector3 { 0.0, 0.0, 1.0 } : Vector3 { 1.0, 0.0, 0.0 };
    return Unit (Cross (normal, seed), result, error);
}

} // namespace

bool MakeArc (const Vector3& centre, const Vector3& normal, double radius, double startRadians, double sweepRadians,
              int segments, std::vector<Vector3>& points, std::string& error)
{
    if (!std::isfinite (radius) || radius <= 0.0) {
        error = "an arc needs a positive radius";
        return false;
    }
    if (!std::isfinite (startRadians) || !std::isfinite (sweepRadians)) {
        error = "an arc needs a finite start angle and sweep";
        return false;
    }
    if (sweepRadians == 0.0) {
        error = "an arc with no sweep has no length";
        return false;
    }
    if (segments < kMinArcSegments || segments > kMaxArcSegments) {
        error = "an arc needs between " + std::to_string (kMinArcSegments) + " and " +
                std::to_string (kMaxArcSegments) + " segments";
        return false;
    }

    Vector3 unitNormal;
    if (!Unit (normal, unitNormal, error)) {
        error = "an arc needs a plane normal with a direction";
        return false;
    }
    Vector3 xAxis;
    if (!PerpendicularTo (unitNormal, xAxis, error))
        return false;
    Vector3 yAxis;
    if (!Unit (Cross (unitNormal, xAxis), yAxis, error))
        return false;

    points.clear ();
    points.reserve (static_cast<std::size_t> (segments) + 1);
    for (int index = 0; index <= segments; ++index) {
        const double angle =
            startRadians + sweepRadians * (static_cast<double> (index) / static_cast<double> (segments));
        const double c = std::cos (angle) * radius;
        const double s = std::sin (angle) * radius;
        points.push_back ({ centre.x + xAxis.x * c + yAxis.x * s, centre.y + xAxis.y * c + yAxis.y * s,
                            centre.z + xAxis.z * c + yAxis.z * s });
    }
    return true;
}

double PolylineLength (const std::vector<Vector3>& points, std::vector<double>& cumulative)
{
    cumulative.clear ();
    cumulative.reserve (points.size ());
    double total = 0.0;
    for (std::size_t index = 0; index < points.size (); ++index) {
        if (index != 0)
            total += Length (Subtract (points[index], points[index - 1]));
        cumulative.push_back (total);
    }
    return total;
}

bool PointOnPolyline (const std::vector<Vector3>& points, double t, Vector3& point, Vector3& tangent,
                      std::string& error)
{
    if (points.size () < 2) {
        error = "a curve needs at least two points";
        return false;
    }
    if (!std::isfinite (t)) {
        error = "the position along the curve is not a finite number";
        return false;
    }

    std::vector<double> cumulative;
    const double total = PolylineLength (points, cumulative);
    if (total <= 0.0) {
        // Every point is in the same place. Not an error - a degenerate curve is
        // a legitimate intermediate result - but there is only one answer.
        point = points.front ();
        tangent = { 1.0, 0.0, 0.0 };
        return true;
    }

    const double clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double target = clamped * total;
    for (std::size_t index = 1; index < points.size (); ++index) {
        if (cumulative[index] < target && index + 1 < points.size ())
            continue;
        const double segmentLength = cumulative[index] - cumulative[index - 1];
        const double along = segmentLength <= 0.0 ? 0.0 : (target - cumulative[index - 1]) / segmentLength;
        const Vector3 delta = Subtract (points[index], points[index - 1]);
        point = Add (points[index - 1], Scale (delta, along));
        if (!Unit (delta, tangent, error))
            tangent = { 1.0, 0.0, 0.0 };
        return true;
    }

    point = points.back ();
    tangent = { 1.0, 0.0, 0.0 };
    return true;
}

bool DividePolyline (const std::vector<Vector3>& points, int count, bool includeEnds, std::vector<Vector3>& result,
                     std::string& error)
{
    if (points.size () < 2) {
        error = "a curve needs at least two points";
        return false;
    }
    if (count < 1 || count > 100000) {
        error = "a curve is divided into between 1 and 100000 segments";
        return false;
    }

    result.clear ();
    // ⚠️ N SEGMENTS IS N+1 POINTS. Dividing a facade into 8 bays wants 9 lines,
    // and an off-by-one here puts every panel half a bay out - which looks like a
    // modelling decision rather than a bug.
    const int samples = includeEnds ? count + 1 : count - 1;
    if (samples <= 0)
        return true; // one segment with the ends excluded has no interior point

    result.reserve (static_cast<std::size_t> (samples));
    for (int index = 0; index < samples; ++index) {
        const double t = includeEnds ? static_cast<double> (index) / static_cast<double> (count)
                                     : static_cast<double> (index + 1) / static_cast<double> (count);
        Vector3 point;
        Vector3 tangent;
        if (!PointOnPolyline (points, t, point, tangent, error))
            return false;
        result.push_back (point);
    }
    return true;
}

} // namespace geomsrv::engine
