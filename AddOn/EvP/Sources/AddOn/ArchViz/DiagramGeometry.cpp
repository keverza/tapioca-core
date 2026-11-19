#include "ArchViz/DiagramGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
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

// Opaque: a diagram body is a body, and a row's alpha is an outline's. 0 is the neutral grey.
uint32_t Opaque (uint32_t rgba)
{
    return ((rgba ? rgba : kDiagramNeutralRgba) & 0xFFFFFF00u) | 0xFFu;
}

// Flat-shaded triangles of one node-local body, and its outline, appended.
void Append (DiagramGeometry& out, const PickMesh& body, uint32_t rgba)
{
    const size_t points = body.positions.size () / 3;
    const size_t first = out.indices.size ();
    for (size_t t = 0; t + 2 < body.indices.size (); t += 3) {
        const uint32_t a = body.indices[t], b = body.indices[t + 1], c = body.indices[t + 2];
        if (a >= points || b >= points || c >= points)
            continue; // a malformed triangle is dropped, never read out of bounds
        const double* p[3] = { &body.positions[a * 3], &body.positions[b * 3], &body.positions[c * 3] };
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
    if (out.indices.size () > first)
        Outline (body, out.lines);
}
} // namespace

namespace {
bool MeshLess (const DiagramMesh& a, const DiagramMesh& b)
{
    return std::tie (a.node, a.detail, a.id) < std::tie (b.node, b.detail, b.id);
}
} // namespace

DiagramGeometry::Range DiagramGeometry::Of (const EntityId& node, Detail detail) const
{
    const auto lo =
        std::lower_bound (meshes.begin (), meshes.end (), std::tie (node, detail),
                          [] (const DiagramMesh& m, const auto& k) { return std::tie (m.node, m.detail) < k; });
    auto hi = lo;
    while (hi != meshes.end () && hi->node == node && hi->detail == detail)
        ++hi;
    return { uint32_t (lo - meshes.begin ()), uint32_t (hi - lo), detail };
}

DiagramGeometry::Range DiagramGeometry::Shown (const EntityId& node, Detail detail) const
{
    if (detail == Detail::Rooms || detail == Detail::Walls)
        if (const auto rooms = Of (node, Detail::Rooms); rooms.count)
            return rooms;
    if (detail != Detail::Topology)
        if (const auto units = Of (node, Detail::Units); units.count)
            return units;
    return Of (node, Detail::Topology);
}

int DiagramGeometry::Find (const EntityId& node, Detail detail) const
{
    const auto range = Of (node, detail);
    return range.count ? int (range.first) : -1;
}

DiagramGeometry BuildGeometry (const BuiltScene& built)
{
    DiagramGeometry out;
    const auto add = [&] (const EntityId& id, const EntityId& node, Detail detail, const PickMesh& body,
                          uint32_t rgba) {
        DiagramMesh mesh;
        mesh.id = id;
        mesh.node = node;
        mesh.detail = detail;
        mesh.firstIndex = uint32_t (out.indices.size ());
        mesh.firstLine = uint32_t (out.lines.size ());
        Append (out, body, Opaque (rgba));
        mesh.indexCount = uint32_t (out.indices.size ()) - mesh.firstIndex;
        mesh.lineCount = uint32_t (out.lines.size ()) - mesh.firstLine;
        if (mesh.indexCount > 0)
            out.meshes.push_back (std::move (mesh));
    };
    for (const auto& shell : built.shells) {
        const auto colour = built.colours.find (shell.id);
        add (shell.id, shell.id, Detail::Topology, shell, colour != built.colours.end () ? colour->second : 0u);
    }
    // Each planned space is a body of its own: its own colour, its own rim, its own pick.
    for (const auto& detail : built.details)
        for (const auto& space : detail.spaces) {
            PickMesh body;
            body.id = space.id;
            body.positions = space.positions;
            body.indices = space.indices;
            add (space.id, detail.id, detail.detail, body, space.rgba);
        }
    std::sort (out.meshes.begin (), out.meshes.end (), MeshLess);
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
                                  const Vec3& origin, Detail detail)
{
    std::vector<DiagramDraw> draws;
    if (frame.poses.size () != plan.nodes.size ())
        return draws; // a frame of another plan: draw nothing rather than misplace
    for (const auto& pose : frame.poses) {
        if (!pose.visible)
            continue;
        const auto range = geometry.Shown (pose.id, detail);
        if (!range.count)
            continue;
        const auto matrix = DisplayMatrix (pose, origin);
        const bool storey = std::binary_search (frame.selected.begin (), frame.selected.end (), pose.id);
        for (uint32_t m = range.first; m < range.first + range.count; ++m) {
            DiagramDraw draw;
            draw.mesh = m;
            draw.translation = { matrix[12], matrix[13], matrix[14] };
            draw.selected =
                storey || (range.detail != Detail::Topology &&
                           std::binary_search (frame.selected.begin (), frame.selected.end (), geometry.meshes[m].id));
            draws.push_back (draw);
        }
    }
    return draws;
}
std::vector<DiagramLineDraw> LineDrawsOf (const DiagramGeometry& geometry, const LayoutPlan& plan, const Frame& frame,
                                          const Vec3& origin, bool outlines, bool connectors, Detail detail)
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
            const auto range = geometry.Shown (pose.id, detail);
            for (uint32_t m = range.first; m < range.first + range.count; ++m) {
                if (geometry.meshes[m].lineCount == 0)
                    continue;
                DiagramLineDraw draw;
                draw.firstLine = geometry.meshes[m].firstLine;
                draw.lineCount = geometry.meshes[m].lineCount;
                draw.first = draw.second = translation (int (i));
                draws.push_back (draw);
            }
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
namespace {
// Nearest ray-triangle distance of a node-local body (Moller-Trumbore, both faces).
bool RayHit (const std::vector<double>& positions, const std::vector<uint32_t>& indices, const Vec3& o, const Vec3& d,
             double& best, int& triangle)
{
    bool hit = false;
    for (size_t f = 0; f + 2 < indices.size (); f += 3) {
        Vec3 v[3];
        bool valid = true;
        for (int c = 0; c < 3 && valid; ++c) {
            const size_t at = size_t (indices[f + size_t (c)]) * 3;
            valid = at + 2 < positions.size ();
            if (valid)
                v[c] = { positions[at], positions[at + 1], positions[at + 2] };
        }
        if (!valid)
            continue;
        const Vec3 e1 { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
        const Vec3 e2 { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
        const Vec3 p { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
        const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::abs (det) < 1e-15)
            continue;
        const double inv = 1.0 / det;
        const Vec3 s { o[0] - v[0][0], o[1] - v[0][1], o[2] - v[0][2] };
        const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < 0 || u > 1)
            continue;
        const Vec3 q { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
        const double w = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (w < 0 || u + w > 1)
            continue;
        const double t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (t > 1e-9 && t < best) {
            best = t;
            triangle = int (f / 3);
            hit = true;
        }
    }
    return hit;
}
} // namespace

std::optional<PickHit> PickShown (const Frame& frame, const LayoutPlan& plan, const DiagramGeometry& geometry,
                                  const std::vector<PickMesh>& shells, const std::vector<DetailMesh>& details,
                                  Detail detail, const Vec3& origin, const Vec3& direction)
{
    const double length =
        std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (!(length > 0) || frame.poses.size () != plan.nodes.size ())
        return std::nullopt;
    const Vec3 d { direction[0] / length, direction[1] / length, direction[2] / length };
    std::map<EntityId, const PickMesh*> shellOf;
    for (const auto& shell : shells)
        shellOf.emplace (shell.id, &shell);
    std::map<std::pair<EntityId, Detail>, const DetailMesh*> detailOf;
    for (const auto& mesh : details)
        detailOf.emplace (std::pair { mesh.id, mesh.detail }, &mesh);
    std::optional<PickHit> best;
    double nearest = std::numeric_limits<double>::infinity ();
    for (size_t i = 0; i < plan.nodes.size (); ++i) {
        const auto& node = plan.nodes[i];
        const auto& pose = frame.poses[i];
        if (!pose.visible || !node.addressable)
            continue;
        const auto shown = geometry.Shown (node.id, detail);
        if (!shown.count)
            continue;
        const Vec3 o { origin[0] - pose.pivot[0] - pose.offset[0], origin[1] - pose.pivot[1] - pose.offset[1],
                       origin[2] - pose.pivot[2] - pose.offset[2] };
        const auto take = [&] (const EntityId& id, const std::vector<double>& positions,
                               const std::vector<uint32_t>& indices) {
            int triangle = -1;
            if (!RayHit (positions, indices, o, d, nearest, triangle))
                return;
            PickHit hit;
            hit.id = id;
            hit.distance = nearest;
            hit.triangle = triangle;
            for (int k = 0; k < 3; ++k)
                hit.point[size_t (k)] = origin[size_t (k)] + d[size_t (k)] * nearest;
            best = hit;
        };
        if (shown.detail == Detail::Topology) {
            if (const auto shell = shellOf.find (node.id); shell != shellOf.end ())
                take (node.id, shell->second->positions, shell->second->indices);
            continue;
        }
        if (const auto mesh = detailOf.find ({ node.id, shown.detail }); mesh != detailOf.end ())
            for (const auto& space : mesh->second->spaces)
                take (space.id, space.positions, space.indices);
    }
    return best;
}
} // namespace geomsrv::archviz::diagram
