#include "Edit/EditEllipse.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::edit {
namespace {
constexpr double kQuarter = 0.55228474983079339840;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadiusTolerance = 1.0e-9;

bool ValidMode (EllipseMode mode)
{
    return mode == EllipseMode::Circle || mode == EllipseMode::Ellipse;
}

double Magnitude (Vec3 value)
{
    return Length (value);
}

bool ValidDefinition (const EllipseDefinition& definition, std::string& error)
{
    if (!ValidMode (definition.mode) || !Finite (definition.center) || !Finite (definition.axisU) ||
        !Finite (definition.axisV)) {
        error = "the ellipse definition is not finite or has an unknown mode";
        return false;
    }
    const double u = Magnitude (definition.axisU), v = Magnitude (definition.axisV);
    const double cross = Magnitude (Cross (definition.axisU, definition.axisV));
    if (!(u >= kMinSegment) || !(v >= kMinSegment) || !(cross >= kMinSegment * std::max (u, v))) {
        error = "an ellipse needs two independent, nonzero axes";
        return false;
    }
    if (definition.mode == EllipseMode::Circle &&
        (std::abs (u - v) > kRadiusTolerance * std::max (u, v) ||
         std::abs (Dot (definition.axisU, definition.axisV)) > kRadiusTolerance * u * v)) {
        error = "a circle needs equal perpendicular axes";
        return false;
    }
    return true;
}

bool PlaneFrame (Vec3 normal, Vec3& u, Vec3& v)
{
    if (!Normalize (normal, normal))
        return false;
    const Vec3 seed = std::abs (normal.z) < 0.9 ? Vec3 { 0.0, 0.0, 1.0 } : Vec3 { 1.0, 0.0, 0.0 };
    if (!Normalize (seed - normal * Dot (seed, normal), u))
        return false;
    return Normalize (Cross (normal, u), v);
}

struct PrincipalAxes {
    Vec3 normal, major, minor;
    double majorRadius = 0.0, minorRadius = 0.0;
};

bool Principal (const EllipseDefinition& definition, PrincipalAxes& axes, std::string& error)
{
    if (!ValidDefinition (definition, error))
        return false;
    const Vec3 a = definition.axisU, b = definition.axisV;
    const double aa = Dot (a, a), bb = Dot (b, b), ab = Dot (a, b);
    const double separation = std::hypot (aa - bb, 2.0 * ab);
    const double largest = 0.5 * (aa + bb + separation);
    // det(A^T A) / largest avoids cancellation for highly eccentric ellipses.
    const double area = Magnitude (Cross (a, b));
    const double smallest = (area / largest) * area;
    double x = 1.0, y = 0.0;
    if (separation > std::numeric_limits<double>::epsilon () * std::max (aa, bb)) {
        x = ab;
        y = largest - aa;
        const double length = std::hypot (x, y);
        if (length > 0.0)
            x /= length, y /= length;
        else
            x = 1.0, y = 0.0;
    }
    if (!Normalize (a * x + b * y, axes.major) || !Normalize (a * -y + b * x, axes.minor) ||
        !Normalize (Cross (a, b), axes.normal)) {
        error = "the ellipse axes cannot be resolved";
        return false;
    }
    axes.majorRadius = std::sqrt (largest);
    axes.minorRadius = std::sqrt (smallest);
    return std::isfinite (axes.majorRadius) && std::isfinite (axes.minorRadius);
}

bool DefinitionFromParameters (const EllipseParameters& parameters, EllipseDefinition& definition, std::string& error)
{
    if (!ValidMode (parameters.mode) || !Finite (parameters.center) || !std::isfinite (parameters.majorRadius) ||
        !std::isfinite (parameters.minorRadius) || !std::isfinite (parameters.rotation)) {
        error = "ellipse parameters must be finite and use a known mode";
        return false;
    }
    if (!(parameters.majorRadius >= kMinSegment) || !(parameters.minorRadius >= kMinSegment)) {
        error = "ellipse radii must be at least 0.000001 m";
        return false;
    }
    if (parameters.mode == EllipseMode::Circle &&
        std::abs (parameters.majorRadius - parameters.minorRadius) >
            kRadiusTolerance * std::max (parameters.majorRadius, parameters.minorRadius)) {
        error = "a circle needs equal radii";
        return false;
    }
    Vec3 u, v, normal;
    if (!Normalize (parameters.normal, normal) || !PlaneFrame (normal, u, v)) {
        error = "the ellipse plane normal must be nonzero";
        return false;
    }
    const double angle = std::remainder (parameters.rotation, 2.0 * kPi);
    const Vec3 major = u * std::cos (angle) + v * std::sin (angle);
    const Vec3 minor = Cross (normal, major);
    const double rU = parameters.mode == EllipseMode::Circle ? 0.5 * (parameters.majorRadius + parameters.minorRadius)
                                                             : parameters.majorRadius;
    const double rV = parameters.mode == EllipseMode::Circle ? rU : parameters.minorRadius;
    definition = { parameters.mode, parameters.center, major * rU, minor * rV };
    return ValidDefinition (definition, error);
}

bool SamePoint (Vec3 a, Vec3 b, double tolerance)
{
    return Distance (a, b) <= tolerance;
}

Vec3 RotateAround (Vec3 vector, Vec3 axis, double angle)
{
    const double c = std::cos (angle), s = std::sin (angle);
    return vector * c + Cross (axis, vector) * s + axis * (Dot (axis, vector) * (1.0 - c));
}

bool LocksAllow (const Entity& before, const Entity& after, std::string& error)
{
    for (size_t i = 0; i < before.vertices.size (); ++i) {
        const auto& old = before.vertices[i];
        const auto& now = after.vertices[i];
        const bool xy = std::abs (now.p.x - old.p.x) > 1.0e-12 || std::abs (now.p.y - old.p.y) > 1.0e-12;
        const bool z = std::abs (now.p.z - old.p.z) > 1.0e-12;
        if ((xy && old.lockXY) || (z && old.lockZ)) {
            error = "a selected vertex is locked";
            return false;
        }
    }
    return true;
}

bool StoreDefinition (Document& doc, Id id, const EllipseDefinition& definition, std::string& error)
{
    Entity* entity = Find (doc, id);
    if (entity == nullptr || !entity->ellipse) {
        error = "that object is not a parametric ellipse";
        return false;
    }
    Entity candidate = *entity;
    candidate.ellipse = definition;
    if (!RegenerateEllipse (candidate, error) || !LocksAllow (*entity, candidate, error))
        return false;
    *entity = std::move (candidate);
    return true;
}

bool StoreParameters (Document& doc, Id id, const EllipseParameters& parameters, std::string& error)
{
    EllipseDefinition definition;
    return DefinitionFromParameters (parameters, definition, error) && StoreDefinition (doc, id, definition, error);
}
} // namespace

bool RegenerateEllipse (Entity& entity, std::string& error)
{
    if (!entity.ellipse) {
        error = "the object has no ellipse definition";
        return false;
    }
    const auto& definition = *entity.ellipse;
    if (!ValidDefinition (definition, error))
        return false;
    if (!entity.closed || entity.kind != Kind::Polyline || entity.vertices.size () != 4 ||
        entity.segments.size () != 4) {
        error = "a parametric ellipse must be a closed four-segment polyline";
        return false;
    }
    const Vec3& c = definition.center;
    const Vec3& a = definition.axisU;
    const Vec3& b = definition.axisV;
    const Vec3 anchors[4] = { c + a, c + b, c - a, c - b };
    const Vec3 tangents[4] = { b * kQuarter, a * -kQuarter, b * -kQuarter, a * kQuarter };
    for (size_t i = 0; i < entity.vertices.size (); ++i) {
        auto& vertex = entity.vertices[i];
        vertex.p = anchors[i];
        vertex.in = tangents[i] * -1.0;
        vertex.out = tangents[i];
        vertex.radius = 0.0;
        vertex.smooth = true;
    }
    error.clear ();
    return true;
}

bool ValidateEllipse (const Entity& entity, std::string& error)
{
    if (!entity.ellipse)
        return true;
    if (!entity.closed || entity.kind != Kind::Polyline || entity.vertices.size () != 4 ||
        entity.segments.size () != 4) {
        error = "a parametric ellipse must be a closed four-segment polyline";
        return false;
    }
    Entity expected = entity;
    if (!RegenerateEllipse (expected, error))
        return false;
    const auto& d = *entity.ellipse;
    const double scale = std::max ({ 1.0, Magnitude (d.axisU), Magnitude (d.axisV) });
    const double coordinate = std::max ({ std::abs (d.center.x), std::abs (d.center.y), std::abs (d.center.z) });
    const double tolerance = 1.0e-10 * scale + 64.0 * std::numeric_limits<double>::epsilon () * coordinate;
    for (size_t i = 0; i < entity.vertices.size (); ++i) {
        const auto& actual = entity.vertices[i];
        const auto& generated = expected.vertices[i];
        if (!SamePoint (actual.p, generated.p, tolerance) || !SamePoint (actual.in, generated.in, tolerance) ||
            !SamePoint (actual.out, generated.out, tolerance) || actual.radius != 0.0 || !actual.smooth) {
            error = "a parametric ellipse's vertices must match its authoritative definition";
            return false;
        }
    }
    error.clear ();
    return true;
}

bool GetEllipseParameters (const Entity& entity, EllipseParameters& parameters, std::string& error)
{
    if (!entity.ellipse) {
        error = "that object is not a parametric ellipse";
        return false;
    }
    PrincipalAxes axes;
    if (!Principal (*entity.ellipse, axes, error))
        return false;
    Vec3 planeU, planeV;
    if (!PlaneFrame (axes.normal, planeU, planeV)) {
        error = "the ellipse plane normal cannot be resolved";
        return false;
    }
    EllipseParameters result;
    result.mode = entity.ellipse->mode;
    result.center = entity.ellipse->center;
    result.normal = axes.normal;
    result.majorRadius = axes.majorRadius;
    result.minorRadius = axes.minorRadius;
    result.rotation = std::atan2 (Dot (axes.major, planeV), Dot (axes.major, planeU));
    parameters = result;
    error.clear ();
    return true;
}

bool SetEllipseParameters (Document& doc, Id entity, const EllipseParameters& parameters, std::string& error)
{
    return StoreParameters (doc, entity, parameters, error);
}

bool SetEllipseCenter (Document& doc, Id id, Vec3 center, std::string& error)
{
    Entity* entity = Find (doc, id);
    if (entity == nullptr || !entity->ellipse) {
        error = "that object is not a parametric ellipse";
        return false;
    }
    EllipseDefinition definition = *entity->ellipse;
    definition.center = center;
    return StoreDefinition (doc, id, definition, error);
}

bool SetEllipseRadii (Document& doc, Id id, double majorRadius, double minorRadius, std::string& error)
{
    Entity* entity = Find (doc, id);
    if (entity == nullptr || !entity->ellipse) {
        error = "that object is not a parametric ellipse";
        return false;
    }
    EllipseParameters parameters;
    if (!GetEllipseParameters (*entity, parameters, error))
        return false;
    parameters.majorRadius = majorRadius;
    parameters.minorRadius = minorRadius;
    return StoreParameters (doc, id, parameters, error);
}

bool SetEllipseRotation (Document& doc, Id id, double radians, std::string& error)
{
    Entity* entity = Find (doc, id);
    if (entity == nullptr || !entity->ellipse) {
        error = "that object is not a parametric ellipse";
        return false;
    }
    EllipseParameters parameters;
    if (!GetEllipseParameters (*entity, parameters, error))
        return false;
    if (!std::isfinite (radians)) {
        error = "ellipse rotation must be finite";
        return false;
    }
    EllipseDefinition definition = *entity->ellipse;
    const double change = std::remainder (radians - parameters.rotation, 2.0 * kPi);
    definition.axisU = RotateAround (definition.axisU, parameters.normal, change);
    definition.axisV = RotateAround (definition.axisV, parameters.normal, change);
    return StoreDefinition (doc, id, definition, error);
}

bool SetEllipseMode (Document& doc, Id id, EllipseMode mode, std::string& error)
{
    Entity* entity = Find (doc, id);
    if (entity == nullptr || !entity->ellipse) {
        error = "that object is not a parametric ellipse";
        return false;
    }
    EllipseParameters parameters;
    if (!GetEllipseParameters (*entity, parameters, error))
        return false;
    if (!ValidMode (mode)) {
        error = "unknown ellipse mode";
        return false;
    }
    if (mode == EllipseMode::Circle) {
        if (std::abs (parameters.majorRadius - parameters.minorRadius) >
            kRadiusTolerance * std::max (parameters.majorRadius, parameters.minorRadius)) {
            error = "set the two radii equal before changing an ellipse to a circle";
            return false;
        }
        parameters.majorRadius = parameters.minorRadius = 0.5 * (parameters.majorRadius + parameters.minorRadius);
        PrincipalAxes axes;
        if (!Principal (*entity->ellipse, axes, error))
            return false;
        EllipseDefinition definition { EllipseMode::Circle, entity->ellipse->center,
                                       axes.major * parameters.majorRadius, axes.minor * parameters.minorRadius };
        return StoreDefinition (doc, id, definition, error);
    }
    EllipseDefinition definition = *entity->ellipse;
    definition.mode = mode;
    return StoreDefinition (doc, id, definition, error);
}

bool ConvertEllipseToFreeform (Document& doc, Id id, std::string& error)
{
    Entity* entity = Find (doc, id);
    if (entity == nullptr || !entity->ellipse) {
        error = "that object is not a parametric ellipse";
        return false;
    }
    entity->ellipse.reset ();
    error.clear ();
    return true;
}

} // namespace geomsrv::edit
