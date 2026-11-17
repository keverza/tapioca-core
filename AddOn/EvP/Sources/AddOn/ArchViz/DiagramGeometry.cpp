#include "ArchViz/DiagramGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::diagram {
int DiagramGeometry::Find (const EntityId& id) const
{
    auto it = std::lower_bound (meshes.begin (), meshes.end (), id,
                                [] (const DiagramMesh& m, const EntityId& key) { return m.id < key; });
    return it != meshes.end () && it->id == id ? int (it - meshes.begin ()) : -1;
}

DiagramGeometry BuildGeometry (const BuiltScene& built)
{
    DiagramGeometry out;
    std::vector<const PickMesh*> shells;
    for (const auto& shell : built.shells)
        shells.push_back (&shell);
    std::sort (shells.begin (), shells.end (), [] (const auto* a, const auto* b) { return a->id < b->id; });
    for (const PickMesh* shell : shells) {
        const auto colour = built.colours.find (shell->id);
        // Opaque: a diagram storey is a body, and the row's alpha is an outline's.
        const uint32_t rgba =
            ((colour != built.colours.end () && colour->second ? colour->second : kDiagramNeutralRgba) & 0xFFFFFF00u) |
            0xFFu;
        DiagramMesh mesh;
        mesh.id = shell->id;
        mesh.firstIndex = uint32_t (out.indices.size ());
        const size_t points = shell->positions.size () / 3;
        for (size_t t = 0; t + 2 < shell->indices.size (); t += 3) {
            const uint32_t a = shell->indices[t], b = shell->indices[t + 1], c = shell->indices[t + 2];
            if (a >= points || b >= points || c >= points)
                continue; // a malformed triangle is dropped, never read out of bounds
            const double* p[3] = { &shell->positions[a * 3], &shell->positions[b * 3], &shell->positions[c * 3] };
            const double u[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
            const double v[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
            double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
            const double len = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(len > 1e-12))
                continue; // degenerate: nothing to see, no normal to shade
            for (double& k : n)
                k /= len;
            for (int corner = 0; corner < 3; ++corner) {
                DiagramVertex vertex {};
                for (int k = 0; k < 3; ++k) {
                    vertex.position[k] = float (p[corner][k]);
                    vertex.normal[k] = float (n[k]);
                }
                vertex.rgba = rgba;
                out.indices.push_back (uint32_t (out.vertices.size ()));
                out.vertices.push_back (vertex);
            }
        }
        mesh.indexCount = uint32_t (out.indices.size ()) - mesh.firstIndex;
        if (mesh.indexCount > 0)
            out.meshes.push_back (std::move (mesh));
    }
    return out;
}

std::vector<DiagramDraw> DrawsOf (const DiagramGeometry& geometry, const LayoutPlan& plan, const Frame& frame,
                                  const Vec3& origin)
{
    std::vector<DiagramDraw> draws;
    if (frame.poses.size () != plan.nodes.size ())
        return draws; // a frame of another plan: draw nothing rather than misplace
    for (const auto& pose : frame.poses) {
        if (!pose.visible)
            continue;
        const int mesh = geometry.Find (pose.id);
        if (mesh < 0)
            continue;
        DiagramDraw draw;
        draw.mesh = uint32_t (mesh);
        const auto matrix = DisplayMatrix (pose, origin);
        draw.translation = { matrix[12], matrix[13], matrix[14] };
        draw.selected = std::binary_search (frame.selected.begin (), frame.selected.end (), pose.id);
        draws.push_back (draw);
    }
    return draws;
}
} // namespace geomsrv::archviz::diagram
