#include "Geometry/GeometryEngine.hpp"

#include <clipper2/clipper.h>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <utility>

namespace geomsrv::engine {
namespace {

constexpr int kClipperPrecision = 6;
constexpr double kPlanarityTolerance = 1.0e-9;

glm::dvec3 ToGlm (const Vector3& value)
{
    return { value.x, value.y, value.z };
}

Vector3 FromGlm (const glm::dvec3& value)
{
    return { value.x, value.y, value.z };
}

bool ToClipperPath (const Polygon& polygon, Clipper2Lib::PathD& path, double& elevation, std::string& error)
{
    if (polygon.points.size () < 3) {
        error = "polygon requires at least three points";
        return false;
    }

    elevation = polygon.points.front ().z;
    path.clear ();
    path.reserve (polygon.points.size ());
    for (const Vector3& point : polygon.points) {
        if (!std::isfinite (point.x) || !std::isfinite (point.y) || !std::isfinite (point.z)) {
            error = "polygon coordinates must be finite";
            return false;
        }
        if (std::abs (point.z - elevation) > kPlanarityTolerance) {
            error = "polygon must lie in a plane parallel to world XY";
            return false;
        }
        path.emplace_back (point.x, point.y);
    }
    return true;
}

std::vector<Polygon> FromClipperPaths (const Clipper2Lib::PathsD& paths, double elevation)
{
    std::vector<Polygon> result;
    result.reserve (paths.size ());
    for (const Clipper2Lib::PathD& path : paths) {
        if (path.size () < 3)
            continue;
        Polygon polygon;
        polygon.points.reserve (path.size ());
        for (const Clipper2Lib::PointD& point : path)
            polygon.points.push_back ({ point.x, point.y, elevation });
        result.push_back (std::move (polygon));
    }
    return result;
}

} // namespace

Vector3 Add (const Vector3& left, const Vector3& right)
{
    return FromGlm (ToGlm (left) + ToGlm (right));
}

Vector3 Cross (const Vector3& left, const Vector3& right)
{
    return FromGlm (glm::cross (ToGlm (left), ToGlm (right)));
}

double Dot (const Vector3& left, const Vector3& right)
{
    return glm::dot (ToGlm (left), ToGlm (right));
}

bool Unit (const Vector3& value, Vector3& result, std::string& error)
{
    const glm::dvec3 vector = ToGlm (value);
    const double length = glm::length (vector);
    if (!std::isfinite (length) || length <= 0.0) {
        error = "cannot normalize a zero or non-finite vector";
        return false;
    }
    result = FromGlm (vector / length);
    error.clear ();
    return true;
}

bool BooleanPolygons (const Polygon& subject, const Polygon& clip, PolygonOperation operation,
                      std::vector<Polygon>& result, std::string& error)
{
    Clipper2Lib::PathD subjectPath;
    Clipper2Lib::PathD clipPath;
    double subjectElevation = 0.0;
    double clipElevation = 0.0;
    if (!ToClipperPath (subject, subjectPath, subjectElevation, error) ||
        !ToClipperPath (clip, clipPath, clipElevation, error))
        return false;
    if (std::abs (subjectElevation - clipElevation) > kPlanarityTolerance) {
        error = "polygons must share the same world XY plane";
        return false;
    }

    Clipper2Lib::ClipType clipType = Clipper2Lib::ClipType::Union;
    if (operation == PolygonOperation::Difference)
        clipType = Clipper2Lib::ClipType::Difference;
    else if (operation == PolygonOperation::Intersection)
        clipType = Clipper2Lib::ClipType::Intersection;

    const Clipper2Lib::PathsD paths = Clipper2Lib::BooleanOp (
        clipType, Clipper2Lib::FillRule::NonZero, { std::move (subjectPath) }, { std::move (clipPath) },
        kClipperPrecision);
    result = FromClipperPaths (paths, subjectElevation);
    error.clear ();
    return true;
}

bool OffsetPolygon (const Polygon& polygon, double distance, std::vector<Polygon>& result, std::string& error)
{
    if (!std::isfinite (distance)) {
        error = "offset distance must be finite";
        return false;
    }

    Clipper2Lib::PathD path;
    double elevation = 0.0;
    if (!ToClipperPath (polygon, path, elevation, error))
        return false;
    const Clipper2Lib::PathsD paths = Clipper2Lib::InflatePaths (
        { std::move (path) }, distance, Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon, 2.0,
        kClipperPrecision);
    result = FromClipperPaths (paths, elevation);
    error.clear ();
    return true;
}

} // namespace geomsrv::engine
