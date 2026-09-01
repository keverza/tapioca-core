#include "Geometry/Transforms.hpp"

#include <cmath>

namespace geomsrv::engine {
namespace {

double Determinant (const Transform& t)
{
    return t.m[0][0] * (t.m[1][1] * t.m[2][2] - t.m[1][2] * t.m[2][1]) -
           t.m[0][1] * (t.m[1][0] * t.m[2][2] - t.m[1][2] * t.m[2][0]) +
           t.m[0][2] * (t.m[1][0] * t.m[2][1] - t.m[1][1] * t.m[2][0]);
}

bool FiniteVector (const Vector3& value)
{
    return std::isfinite (value.x) && std::isfinite (value.y) && std::isfinite (value.z);
}

} // namespace

Transform Identity ()
{
    return Transform {};
}

Transform Translation (const Vector3& by)
{
    Transform result;
    result.m[0][3] = by.x;
    result.m[1][3] = by.y;
    result.m[2][3] = by.z;
    return result;
}

bool Rotation (const Vector3& origin, const Vector3& axis, double radians, Transform& result, std::string& error)
{
    if (!FiniteVector (origin) || !FiniteVector (axis) || !std::isfinite (radians)) {
        error = "a rotation needs a finite origin, axis and angle";
        return false;
    }
    Vector3 unit;
    if (!Unit (axis, unit, error)) {
        // A zero-length axis is refused rather than treated as identity: a
        // rotation node that quietly does nothing looks like a wiring mistake
        // somewhere else entirely.
        error = "a rotation needs an axis with a direction";
        return false;
    }

    // Rodrigues, written out. The rotation is about the axis through `origin`,
    // so it is conjugated by the translation rather than applied at the world
    // origin - rotating a facade about the world origin instead of about its own
    // corner is the commonest thing to get wrong here, and it moves the geometry
    // a long way rather than a little.
    const double c = std::cos (radians);
    const double s = std::sin (radians);
    const double t = 1.0 - c;
    const double x = unit.x;
    const double y = unit.y;
    const double z = unit.z;

    double linear[3][3];
    linear[0][0] = t * x * x + c;
    linear[0][1] = t * x * y - s * z;
    linear[0][2] = t * x * z + s * y;
    linear[1][0] = t * x * y + s * z;
    linear[1][1] = t * y * y + c;
    linear[1][2] = t * y * z - s * x;
    linear[2][0] = t * x * z - s * y;
    linear[2][1] = t * y * z + s * x;
    linear[2][2] = t * z * z + c;

    result = Transform {};
    const double centre[3] = { origin.x, origin.y, origin.z };
    for (int row = 0; row < 3; ++row) {
        double shifted = centre[row];
        for (int column = 0; column < 3; ++column) {
            result.m[row][column] = linear[row][column];
            shifted -= linear[row][column] * centre[column];
        }
        result.m[row][3] = shifted;
    }
    return true;
}

bool Scaling (const Vector3& origin, const Vector3& factors, Transform& result, std::string& error)
{
    if (!FiniteVector (origin) || !FiniteVector (factors)) {
        error = "a scale needs a finite origin and factor";
        return false;
    }
    // Zero collapses geometry onto a plane and produces degenerate triangles
    // rather than an error, so it is refused - it is far more often a typo or an
    // unwired input than a request.
    if (factors.x == 0.0 || factors.y == 0.0 || factors.z == 0.0) {
        error = "a scale factor cannot be zero";
        return false;
    }

    result = Transform {};
    const double centre[3] = { origin.x, origin.y, origin.z };
    const double scale[3] = { factors.x, factors.y, factors.z };
    for (int row = 0; row < 3; ++row) {
        result.m[row][row] = scale[row];
        result.m[row][3] = centre[row] - scale[row] * centre[row];
    }
    return true;
}

bool Mirroring (const Vector3& origin, const Vector3& normal, Transform& result, std::string& error)
{
    if (!FiniteVector (origin) || !FiniteVector (normal)) {
        error = "a mirror needs a finite plane origin and normal";
        return false;
    }
    Vector3 unit;
    if (!Unit (normal, unit, error)) {
        error = "a mirror needs a plane normal with a direction";
        return false;
    }

    // I - 2nn^T, conjugated by the plane's origin.
    result = Transform {};
    const double n[3] = { unit.x, unit.y, unit.z };
    const double centre[3] = { origin.x, origin.y, origin.z };
    for (int row = 0; row < 3; ++row) {
        double shifted = centre[row];
        for (int column = 0; column < 3; ++column) {
            const double value = (row == column ? 1.0 : 0.0) - 2.0 * n[row] * n[column];
            result.m[row][column] = value;
            shifted -= value * centre[column];
        }
        result.m[row][3] = shifted;
    }
    return true;
}

Transform Compose (const Transform& second, const Transform& first)
{
    Transform result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k)
                sum += second.m[row][k] * first.m[k][column];
            result.m[row][column] = sum;
        }
        double sum = second.m[row][3];
        for (int k = 0; k < 3; ++k)
            sum += second.m[row][k] * first.m[k][3];
        result.m[row][3] = sum;
    }
    return result;
}

Vector3 ApplyToPoint (const Transform& t, const Vector3& p)
{
    return { t.m[0][0] * p.x + t.m[0][1] * p.y + t.m[0][2] * p.z + t.m[0][3],
             t.m[1][0] * p.x + t.m[1][1] * p.y + t.m[1][2] * p.z + t.m[1][3],
             t.m[2][0] * p.x + t.m[2][1] * p.y + t.m[2][2] * p.z + t.m[2][3] };
}

Vector3 ApplyToDirection (const Transform& t, const Vector3& d)
{
    // No translation. A direction is a difference of two points, and the
    // translation cancels in that difference.
    return { t.m[0][0] * d.x + t.m[0][1] * d.y + t.m[0][2] * d.z, t.m[1][0] * d.x + t.m[1][1] * d.y + t.m[1][2] * d.z,
             t.m[2][0] * d.x + t.m[2][1] * d.y + t.m[2][2] * d.z };
}

Vector3 ApplyToNormal (const Transform& t, const Vector3& n)
{
    const double det = Determinant (t);
    if (det == 0.0)
        return n; // unreachable through the builders above, all of which refuse degeneracy

    // The inverse transpose, built from the cofactors so no separate inverse is
    // needed. Scaled by 1/det, which does not change the direction; the caller
    // re-normalises if it cares about length.
    const double c[3][3] = {
        { t.m[1][1] * t.m[2][2] - t.m[1][2] * t.m[2][1], t.m[1][2] * t.m[2][0] - t.m[1][0] * t.m[2][2],
          t.m[1][0] * t.m[2][1] - t.m[1][1] * t.m[2][0] },
        { t.m[0][2] * t.m[2][1] - t.m[0][1] * t.m[2][2], t.m[0][0] * t.m[2][2] - t.m[0][2] * t.m[2][0],
          t.m[0][1] * t.m[2][0] - t.m[0][0] * t.m[2][1] },
        { t.m[0][1] * t.m[1][2] - t.m[0][2] * t.m[1][1], t.m[0][2] * t.m[1][0] - t.m[0][0] * t.m[1][2],
          t.m[0][0] * t.m[1][1] - t.m[0][1] * t.m[1][0] },
    };
    // inverse-transpose = (adj/det)^T = C/det, so this multiplies by C directly
    // rather than by its transpose. Getting that backwards is invisible for a
    // rotation - where the two agree - and wrong for exactly the non-uniform
    // scale the function exists for.
    return { (c[0][0] * n.x + c[0][1] * n.y + c[0][2] * n.z) / det,
             (c[1][0] * n.x + c[1][1] * n.y + c[1][2] * n.z) / det,
             (c[2][0] * n.x + c[2][1] * n.y + c[2][2] * n.z) / det };
}

bool FlipsOrientation (const Transform& t)
{
    return Determinant (t) < 0.0;
}

} // namespace geomsrv::engine
