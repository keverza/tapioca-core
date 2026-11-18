#include "ArchViz/DiagramGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace geomsrv::archviz::diagram {
namespace {
// Positions closer than this weld into one outline vertex: slice chains repeat a corner
// per face and carry micrometre near-duplicates, and an unwelded edge would read as a
// boundary and draw a line across a face.
constexpr double kWeld = 1e-4; // m

using Cell = std::tuple<long long, long long, long long>;

// Welds by distance, not by cell: two points a micrometre apart can round into
// neighbouring cells, so a point is looked up in its cell and the 26 around it.
class Welder {
  public:
    uint32_t Add (const double* p)
    {
        const long long c[3] = { std::llround (p[0] / kWeld), std::llround (p[1] / kWeld),
                                 std::llround (p[2] / kWeld) };
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy)
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto found = cells_.find ({ c[0] + dx, c[1] + dy, c[2] + dz });
                    if (found == cells_.end ())
                        continue;
                    for (const uint32_t id : found->second) {
                        const double* q = points_[id];
                        if (std::abs (p[0] - q[0]) <= kWeld && std::abs (p[1] - q[1]) <= kWeld &&
                            std::abs (p[2] - q[2]) <= kWeld)
                            return id;
                    }
                }
        const auto id = uint32_t (points_.size ());
        points_.push_back (p);
        cells_[{ c[0], c[1], c[2] }].push_back (id);
        return id;
    }

  private:
    std::map<Cell, std::vector<uint32_t>> cells_;
    std::vector<const double*> points_;
};

struct EdgeFaces {
    std::array<double, 3> normal {}; // the first face's
    const double* a = nullptr;       // the first occurrence's ends, for the drawn line
    const double* b = nullptr;
    int faces = 0;
    bool crease = false;
};

void PushLine (std::vector<DiagramLineVertex>& lines, const double* a, const double* b)
{
    for (const double* p : { a, b }) {
        DiagramLineVertex v;
        for (int k = 0; k < 3; ++k)
            v.position[k] = float (p[k]);
        lines.push_back (v);
    }
}

// Boundary, crease and non-manifold edges of one node-local shell, as a line list.
void Outline (const PickMesh& shell, std::vector<DiagramLineVertex>& lines)
{
    const double creaseCos = std::cos (kOutlineCreaseDegrees * 3.14159265358979323846 / 180.0);
    Welder welder;
    const size_t points = shell.positions.size () / 3;
    std::vector<uint32_t> weldOf (points);
    for (size_t i = 0; i < points; ++i)
        weldOf[i] = welder.Add (&shell.positions[i * 3]);
    std::map<std::pair<uint32_t, uint32_t>, EdgeFaces> edges;
    for (size_t t = 0; t + 2 < shell.indices.size (); t += 3) {
        const uint32_t c[3] = { shell.indices[t], shell.indices[t + 1], shell.indices[t + 2] };
        if (c[0] >= points || c[1] >= points || c[2] >= points)
            continue;
        // A sliver whose corners weld together is no face: counting it would give its long
        // edge two extra faces, and a non-manifold edge is drawn.
        if (weldOf[c[0]] == weldOf[c[1]] || weldOf[c[1]] == weldOf[c[2]] || weldOf[c[0]] == weldOf[c[2]])
            continue;
        const double* p[3] = { &shell.positions[c[0] * 3], &shell.positions[c[1] * 3], &shell.positions[c[2] * 3] };
        const double u[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
        const double v[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
        std::array<double, 3> n = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const double len = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(len > 1e-12))
            continue;
        for (double& k : n)
            k /= len;
        for (int e = 0; e < 3; ++e) {
            const uint32_t wa = weldOf[c[e]], wb = weldOf[c[(e + 1) % 3]];
            auto& edge = edges[{ std::min (wa, wb), std::max (wa, wb) }];
            if (edge.faces == 0) {
                edge.normal = n;
                edge.a = p[e];
                edge.b = p[(e + 1) % 3];
            }
            // Winding is not oriented (DiagramLayer culls nothing): compare unsigned.
            else if (std::abs (n[0] * edge.normal[0] + n[1] * edge.normal[1] + n[2] * edge.normal[2]) < creaseCos)
                edge.crease = true;
            ++edge.faces;
        }
    }
    // An edge with one face is a boundary only off a cap. A storey shell is closed, so a
    // cap's real rim always meets a wall; an unpartnered cap edge is a T-junction of the
    // cap triangulation (a vertex of its wall or a neighbour mid-edge), and drawing it puts
    // a line across the roof. Where the wall side is split instead, the wall's own edges
    // carry the rim.
    for (const auto& [key, edge] : edges) {
        const bool capOnly = edge.faces == 1 && std::abs (edge.normal[2]) > 0.99;
        if ((edge.faces != 2 || edge.crease) && !capOnly)
            PushLine (lines, edge.a, edge.b);
    }
}
} // namespace

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
        mesh.firstLine = uint32_t (out.lines.size ());
        if (mesh.indexCount > 0)
            Outline (*shell, out.lines);
        mesh.lineCount = uint32_t (out.lines.size ()) - mesh.firstLine;
        if (mesh.indexCount > 0)
            out.meshes.push_back (std::move (mesh));
    }
    return out;
}

void AddConnectors (DiagramGeometry& geometry, const LayoutPlan& plan)
{
    geometry.connectors.clear ();
    // Stacked siblings by parent, in rank order: rank k sits on rank k - 1.
    std::map<int, std::vector<int>> stacks;
    for (size_t i = 0; i < plan.nodes.size (); ++i)
        if (plan.nodes[i].stacked)
            stacks[plan.nodes[i].parent].push_back (int (i));
    for (auto& [parent, kids] : stacks) {
        std::stable_sort (kids.begin (), kids.end (), [&] (int a, int b) {
            return plan.nodes[size_t (a)].sibling < plan.nodes[size_t (b)].sibling;
        });
        for (size_t k = 1; k < kids.size (); ++k) {
            const PlanNode& upper = plan.nodes[size_t (kids[k])];
            const PlanNode& lower = plan.nodes[size_t (kids[k - 1])];
            const int mesh = geometry.Find (upper.id);
            if (mesh < 0 || geometry.Find (lower.id) < 0)
                continue;
            const DiagramMesh m = geometry.meshes[size_t (mesh)]; // a copy: `lines` grows below
            const float lowerTop = float (lower.boundsMax[2] - lower.pivot[2]);
            DiagramConnector connector;
            connector.upper = upper.id;
            connector.lower = lower.id;
            connector.firstLine = uint32_t (geometry.lines.size ());
            std::vector<std::array<float, 2>> corners;
            for (uint32_t i = m.firstLine; i + 1 < m.firstLine + m.lineCount; i += 2) {
                const DiagramLineVertex a = geometry.lines[i], b = geometry.lines[i + 1];
                const float dx = b.position[0] - a.position[0], dy = b.position[1] - a.position[1],
                            dz = b.position[2] - a.position[2];
                const float len = std::sqrt (dx * dx + dy * dy + dz * dz);
                if (!(len > 1e-3f) || std::abs (dz) < 0.99f * len)
                    continue; // only the vertical edges are a footprint's corners
                const std::array<float, 2> xy { a.position[0], a.position[1] };
                const bool seen = std::any_of (corners.begin (), corners.end (), [&] (const auto& c) {
                    return std::abs (c[0] - xy[0]) < 0.01f && std::abs (c[1] - xy[1]) < 0.01f;
                });
                if (seen)
                    continue;
                corners.push_back (xy);
                DiagramLineVertex top, bottom;
                top.position[0] = xy[0];
                top.position[1] = xy[1];
                top.position[2] = std::min (a.position[2], b.position[2]);
                top.end = 0;
                // The same world xy in the lower storey's frame; stacked storeys move only in z.
                bottom.position[0] = float (double (xy[0]) + upper.pivot[0] - lower.pivot[0]);
                bottom.position[1] = float (double (xy[1]) + upper.pivot[1] - lower.pivot[1]);
                bottom.position[2] = lowerTop;
                bottom.end = 1;
                geometry.lines.push_back (top);
                geometry.lines.push_back (bottom);
            }
            connector.lineCount = uint32_t (geometry.lines.size ()) - connector.firstLine;
            if (connector.lineCount > 0)
                geometry.connectors.push_back (std::move (connector));
        }
    }
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
std::vector<DiagramLineDraw> LineDrawsOf (const DiagramGeometry& geometry, const LayoutPlan& plan, const Frame& frame,
                                          const Vec3& origin, bool outlines, bool connectors)
{
    std::vector<DiagramLineDraw> draws;
    if (frame.poses.size () != plan.nodes.size ())
        return draws;
    const auto translation = [&] (int node) {
        const auto matrix = DisplayMatrix (frame.poses[size_t (node)], origin);
        return std::array<float, 3> { matrix[12], matrix[13], matrix[14] };
    };
    if (outlines)
        for (size_t i = 0; i < frame.poses.size (); ++i) {
            const auto& pose = frame.poses[i];
            if (!pose.visible)
                continue;
            const int mesh = geometry.Find (pose.id);
            if (mesh < 0 || geometry.meshes[size_t (mesh)].lineCount == 0)
                continue;
            DiagramLineDraw draw;
            draw.firstLine = geometry.meshes[size_t (mesh)].firstLine;
            draw.lineCount = geometry.meshes[size_t (mesh)].lineCount;
            draw.first = draw.second = translation (int (i));
            draws.push_back (draw);
        }
    if (connectors)
        for (const auto& connector : geometry.connectors) {
            const int upper = plan.Find (connector.upper), lower = plan.Find (connector.lower);
            if (upper < 0 || lower < 0 || !frame.poses[size_t (upper)].visible || !frame.poses[size_t (lower)].visible)
                continue;
            DiagramLineDraw draw;
            draw.firstLine = connector.firstLine;
            draw.lineCount = connector.lineCount;
            draw.first = translation (upper);
            draw.second = translation (lower);
            const auto& u = plan.nodes[size_t (upper)];
            const auto& l = plan.nodes[size_t (lower)];
            const double upperBottom = double (draw.first[2]) + (u.boundsMin[2] - u.pivot[2]);
            const double lowerTop = double (draw.second[2]) + (l.boundsMax[2] - l.pivot[2]);
            if (upperBottom - lowerTop < 0.01)
                continue; // assembled: nothing to connect
            draw.dashOrigin = float (upperBottom);
            draw.dashed = true;
            draws.push_back (draw);
        }
    return draws;
}
} // namespace geomsrv::archviz::diagram
