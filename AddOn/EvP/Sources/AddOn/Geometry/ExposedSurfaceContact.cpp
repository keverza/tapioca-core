#include "ExposedSurfaceInternal.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::exposed::detail {
namespace {
struct Corner {
    cp::PointD point;
    double distance;
};
// Clip in the face-normal direction only. Projecting the resulting actual
// surface patch covers no area beyond its boundary (unlike a polygon offset).
std::vector<Corner> Clip (const std::vector<Corner>& input, double boundary, bool above)
{
    std::vector<Corner> output;
    if (input.empty ())
        return output;
    Corner previous = input.back ();
    const auto inside = [&] (double distance) { return above ? distance >= boundary : distance <= boundary; };
    for (const auto& current : input) {
        const bool before = inside (previous.distance), now = inside (current.distance);
        if (before != now) {
            const double t = (boundary - previous.distance) / (current.distance - previous.distance);
            output.push_back ({ { previous.point.x + t * (current.point.x - previous.point.x),
                                  previous.point.y + t * (current.point.y - previous.point.y) },
                                boundary });
        }
        if (now)
            output.push_back (current);
        previous = current;
    }
    return output;
}
} // namespace

bool NearContact (const Face& face, const Body& body, double tolerance, cp::PathsD& paths, Work& work,
                  std::string& error)
{
    const auto& mesh = body.mesh;
    for (size_t i = 0; i < mesh.triangles.size (); i += 3) {
        if (!work.Check (1, error))
            return false;
        const V a = Point (mesh.vertices, mesh.triangles[i]), b = Point (mesh.vertices, mesh.triangles[i + 1]),
                c = Point (mesh.vertices, mesh.triangles[i + 2]);
        const V ab { b.x - a.x, b.y - a.y, b.z - a.z }, ac { c.x - a.x, c.y - a.y, c.z - a.z };
        const auto normal = engine::Cross (ab, ac);
        // Only the solid boundary facing the target can close a gap. A back
        // surface behind a thin target or a same-facing foreground patch cannot.
        if (engine::Dot (normal, face.frame.normal) >= -1e-12 * std::sqrt (engine::Dot (normal, normal)))
            continue;
        std::vector<Corner> polygon;
        for (const auto& p : { a, b, c }) {
            const V delta { p.x - face.frame.origin.x, p.y - face.frame.origin.y, p.z - face.frame.origin.z };
            polygon.push_back ({ face.frame.Project (p), engine::Dot (delta, face.frame.normal) });
        }
        polygon = Clip (polygon, -surfacecoverage::kPlaneTolerance, true);
        polygon = Clip (polygon, tolerance + 1e-12, false);
        if (polygon.size () < 3)
            continue;
        cp::PathD path;
        for (const auto& point : polygon)
            path.push_back (point.point);
        const double area = cp::Area (path);
        if (area == 0)
            continue;
        if (area < 0)
            std::reverse (path.begin (), path.end ());
        paths.push_back (std::move (path));
    }
    return work.Check (0, error);
}
} // namespace geomsrv::exposed::detail
