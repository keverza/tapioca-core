#include "Edit/EditPrimitives.hpp"
#include <cmath>

namespace geomsrv::edit {

namespace {
constexpr double kPi = 3.14159265358979323846;
// A quarter ellipse's cubic handle, as a fraction of its semi-axis: the tangent length that puts
// the curve's midpoint on the true ellipse (4/3 (sqrt 2 - 1)).
constexpr double kQuarter = 0.55228474983079339840;
constexpr double kMinimum = 1.0e-6;

double Along (Vec3 d, Vec3 axis)
{
    return d.x * axis.x + d.y * axis.y + d.z * axis.z;
}
} // namespace

bool OutlinePrimitive (const PrimitiveInput& in, PrimitiveOutline& out, std::string& error)
{
    PrimitiveOutline shape;
    const Vec3 d = in.second - in.first;
    double du = Along (d, in.u), dv = Along (d, in.v);
    switch (in.kind) {
        case Primitive::Rectangle: {
            if (in.uniform) {
                const double side = std::max (std::abs (du), std::abs (dv));
                du = std::copysign (side, du), dv = std::copysign (side, dv);
            }
            if (std::abs (du) < kMinimum || std::abs (dv) < kMinimum) {
                error = "A rectangle needs width and depth.";
                return false;
            }
            const Vec3 a = in.first, b = a + in.u * du, c = b + in.v * dv, e = a + in.v * dv;
            // Counter-clockwise whichever way the corner was dragged.
            shape.points = du * dv > 0 ? std::vector<Vec3> { a, b, c, e } : std::vector<Vec3> { a, e, c, b };
            break;
        }
        case Primitive::Polygon: {
            if (in.sides < 3 || in.sides > kMaxPolygonSides) {
                error = "A polygon has 3 to " + std::to_string (kMaxPolygonSides) + " sides.";
                return false;
            }
            const double radius = std::hypot (du, dv);
            if (radius < kMinimum) {
                error = "A polygon needs a radius.";
                return false;
            }
            // The second click is a vertex: the polygon turns with it.
            const double start = std::atan2 (dv, du);
            for (int k = 0; k < in.sides; ++k) {
                const double t = start + 2.0 * kPi * k / in.sides;
                shape.points.push_back (in.first + in.u * (radius * std::cos (t)) + in.v * (radius * std::sin (t)));
            }
            break;
        }
        case Primitive::Ellipse: {
            double rx = std::abs (du), ry = std::abs (dv);
            if (in.uniform)
                rx = ry = std::hypot (du, dv);
            if (rx < kMinimum || ry < kMinimum) {
                error = in.uniform ? "A circle needs a radius." : "An ellipse needs both radii.";
                return false;
            }
            // Four smooth anchors on the axes, each handle tangent: +u, +v, -u, -v.
            const Vec3 ax = in.u * rx, ay = in.v * ry;
            const Vec3 anchors[4] = { in.first + ax, in.first + ay, in.first - ax, in.first - ay };
            const Vec3 tangents[4] = { ay * kQuarter, ax * -kQuarter, ay * -kQuarter, ax * kQuarter };
            for (int k = 0; k < 4; ++k) {
                shape.points.push_back (anchors[k]);
                shape.out.push_back (tangents[k]);
                shape.in.push_back (tangents[k] * -1.0);
            }
            break;
        }
    }
    out = std::move (shape);
    return true;
}

Id AddPrimitive (Document& doc, const PrimitiveInput& input, const std::string& role, std::string& error)
{
    PrimitiveOutline shape;
    if (!OutlinePrimitive (input, shape, error))
        return kNoId;
    const Id id = AddPolyline (doc, shape.points, true, role);
    Entity* e = Find (doc, id);
    if (e == nullptr) {
        error = "The shape could not be added.";
        return kNoId;
    }
    for (size_t i = 0; i < shape.out.size () && i < e->vertices.size (); ++i) {
        e->vertices[i].in = shape.in[i];
        e->vertices[i].out = shape.out[i];
        e->vertices[i].smooth = true;
    }
    return id;
}

} // namespace geomsrv::edit
