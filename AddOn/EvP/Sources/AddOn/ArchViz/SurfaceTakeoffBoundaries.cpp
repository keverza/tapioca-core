// OVERLAY-INVARIANTS.md: layer content only, no camera/depth/renderer changes.
#include "SurfaceTakeoffLayer.hpp"
#include "Geometry/SurfaceCoverage.hpp"
#include <cmath>

namespace geomsrv::archviz::surfacetakeoff {
namespace {
using V = engine::Vector3;
namespace cp = Clipper2Lib;
V Point (const std::vector<double>& points, uint32_t at)
{
    return { points[size_t (at) * 3], points[size_t (at) * 3 + 1], points[size_t (at) * 3 + 2] };
}
struct Group {
    int32_t surface;
    surfacecoverage::Frame frame;
    cp::PathsD triangles;
};
bool OnPlane (const overlaylayers::Mesh& mesh, const Group& group, const V& normal)
{
    if (engine::Dot (normal, group.frame.normal) < 1 - 1e-10)
        return false;
    for (size_t i = 0; i < mesh.points.size (); i += 3) {
        const V delta { mesh.points[i] - group.frame.origin.x, mesh.points[i + 1] - group.frame.origin.y,
                        mesh.points[i + 2] - group.frame.origin.z };
        if (std::abs (engine::Dot (delta, group.frame.normal)) > surfacecoverage::kPlaneTolerance)
            return false;
    }
    return true;
}
} // namespace
bool BoundaryLines (const Presentation& presentation, const std::vector<size_t>& meshes,
                    std::vector<overlaylayers::Polyline>& lines, std::string& error)
{
    lines.clear ();
    error.clear ();
    std::vector<Group> groups;
    for (size_t i : meshes) {
        const auto& mesh = presentation.layer.meshes[i];
        surfacecoverage::Frame frame;
        if (!surfacecoverage::MakeFrame (Point (mesh.points, mesh.indices[0]), Point (mesh.points, mesh.indices[1]),
                                         Point (mesh.points, mesh.indices[2]), frame)) {
            error = "Cannot form a boundary plane.";
            return false;
        }
        Group* group = nullptr;
        for (auto& candidate : groups)
            if (candidate.surface == presentation.meshSurfaces[i] && OnPlane (mesh, candidate, frame.normal)) {
                group = &candidate;
                break;
            }
        if (!group) {
            groups.push_back ({ presentation.meshSurfaces[i], frame, {} });
            group = &groups.back ();
        }
        for (size_t t = 0; t < mesh.indices.size (); t += 3)
            group->triangles.push_back ({ group->frame.Project (Point (mesh.points, mesh.indices[t])),
                                          group->frame.Project (Point (mesh.points, mesh.indices[t + 1])),
                                          group->frame.Project (Point (mesh.points, mesh.indices[t + 2])) });
    }
    std::vector<overlaylayers::Polyline> prepared;
    for (const auto& group : groups) {
        cp::PathsD boundary;
        if (!surfacecoverage::Boolean (cp::ClipType::Union, group.triangles, {}, boundary, error))
            return false; // no partial display outlines; the UI reports failure separately from areas
        for (const auto& loop : boundary) {
            overlaylayers::Polyline line;
            line.closed = true;
            line.rgba = (kHighlightRgba & 0xFFFFFF00u) | 0x80u;
            line.behind = overlaylayers::Behind::Show;
            line.widthPixels = 1.5f;
            for (const auto& point : loop) {
                const V world = group.frame.Lift (point);
                line.points.insert (line.points.end (), { world.x, world.y, world.z });
            }
            prepared.push_back (std::move (line));
        }
    }
    lines = std::move (prepared);
    return true;
}
} // namespace geomsrv::archviz::surfacetakeoff
