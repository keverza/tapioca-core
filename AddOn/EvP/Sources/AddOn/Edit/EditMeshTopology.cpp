#include "Edit/EditMeshTopology.hpp"

#include "Geometry/TriangleDegeneracy.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace tapioca::geometry {
namespace {

// A boundary half-edge (a -> b): the directed edge of a face whose underlying
// undirected edge has exactly one adjacent face.
struct HalfEdge {
    uint32_t from = 0, to = 0;
};

} // namespace

bool BuildMeshTopology (std::span<const double> positions, std::span<const uint32_t> triangles, MeshTopology& out,
                        std::string& error)
{
    if (positions.size () % 3 != 0) {
        error = "position count is not a multiple of 3";
        return false;
    }
    if (triangles.size () % 3 != 0) {
        error = "triangle index count is not a multiple of 3";
        return false;
    }
    const uint32_t vertexCount = static_cast<uint32_t> (positions.size () / 3);
    for (uint32_t index : triangles)
        if (index >= vertexCount) {
            error = "triangle index out of range";
            return false;
        }

    MeshTopology topo;
    topo.vertexCount = vertexCount;

    // A triangle carries no edge topology when its vertex index repeats or its
    // area is roundoff-zero (non-finite positions are NOT classified here, the
    // same deliberate choice as TriangleDegeneracy.hpp).
    auto isExcluded = [&] (uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || c == a)
            return true;
        return geomsrv::IsRoundoffZeroTriangle (positions.data () + 3 * a, positions.data () + 3 * b,
                                                positions.data () + 3 * c);
    };

    // Incidence counts per canonical undirected edge, accumulated in triangle
    // order. std::map keeps every later iteration order deterministic.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> faceCount;
    for (size_t t = 0; t + 2 < triangles.size (); t += 3) {
        const uint32_t a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        if (isExcluded (a, b, c)) {
            ++topo.degenerateFaceCount;
            continue;
        }
        ++topo.triangleCount;
        ++faceCount[{ std::min (a, b), std::max (a, b) }];
        ++faceCount[{ std::min (b, c), std::max (b, c) }];
        ++faceCount[{ std::min (c, a), std::max (c, a) }];
    }
    topo.edgeCount = faceCount.size ();
    for (const auto& [edge, count] : faceCount)
        if (count == 1)
            ++topo.boundaryEdgeCount;
        else if (count == 2)
            ++topo.interiorEdgeCount;
        else
            ++topo.nonManifoldEdgeCount;

    // Boundary loop tracing: at each boundary vertex the outgoing half-edge is
    // unique on a manifold rim. Walk it until the start closes the loop.
    std::map<uint32_t, uint32_t> outgoing; // vertex -> next vertex along the rim
    for (size_t i = 0; i + 2 < triangles.size (); i += 3) {
        const uint32_t corner[3] = { triangles[i], triangles[i + 1], triangles[i + 2] };
        if (isExcluded (corner[0], corner[1], corner[2]))
            continue;
        for (uint32_t e = 0; e < 3; ++e) {
            const uint32_t from = corner[e], to = corner[(e + 1) % 3];
            if (faceCount[{ std::min (from, to), std::max (from, to) }] == 1)
                outgoing.emplace (from, to);
        }
    }

    std::map<std::pair<uint32_t, uint32_t>, bool> visited;
    for (const auto& [from, to] : outgoing) {
        if (visited[{ from, to }])
            continue;
        std::vector<uint32_t> loop;
        uint32_t at = from;
        bool closed = false;
        // One pass visits every rim half-edge at most once, so the walk cannot
        // loop forever: it either closes at the start or exhausts the rim.
        for (size_t guard = 0; guard <= outgoing.size (); ++guard) {
            auto next = outgoing.find (at);
            if (next == outgoing.end ())
                break;
            if (visited[{ next->first, next->second }])
                break;
            visited[{ next->first, next->second }] = true;
            loop.push_back (next->first);
            at = next->second;
            if (at == from) {
                closed = true;
                break;
            }
        }
        if (closed)
            topo.boundaryLoops.push_back (std::move (loop));
        else
            topo.untracedBoundaryHalfEdges += loop.size ();
    }

    topo.eulerCharacteristic = static_cast<int64_t> (topo.vertexCount) - static_cast<int64_t> (topo.edgeCount) +
                               static_cast<int64_t> (topo.triangleCount);
    if (topo.IsManifold ()) {
        // For a manifold complex 2 - chi - b is even; integer division is exact.
        topo.genus = static_cast<int32_t> (
            (2 - topo.eulerCharacteristic - static_cast<int64_t> (topo.boundaryLoops.size ())) / 2);
    }
    out = std::move (topo);
    return true;
}

} // namespace tapioca::geometry
