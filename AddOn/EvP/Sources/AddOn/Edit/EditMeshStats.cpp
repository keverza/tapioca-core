#include "Edit/EditMeshStats.hpp"

#include "Geometry/TriangleDegeneracy.hpp"

#include <algorithm>
#include <array>
#include <map>

namespace tapioca::geometry {
namespace {

// True if `corner` equals one of `a`, `b`, `c` — a repeated vertex index.
bool HasRepeatedVertex (uint32_t a, uint32_t b, uint32_t c)
{
    return a == b || b == c || c == a;
}

} // namespace

bool ComputeMeshStats (std::span<const double> positions,
                       std::span<const uint32_t> triangles,
                       MeshStats& out,
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

    MeshStats stats;

    // Coincidence classes keyed by the sorted vertex triple. Each entry counts
    // the members of each cyclic orientation, so duplicate and interior
    // numbers are exact for classes of any size. std::map order is
    // deterministic; the counters do not depend on it anyway.
    std::map<std::array<uint32_t, 3>, std::array<uint32_t, 2>> classes;
    std::vector<uint8_t> referenced (vertexCount, 0);
    for (size_t t = 0; t + 2 < triangles.size (); t += 3) {
        const uint32_t a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        if (HasRepeatedVertex (a, b, c)) {
            ++stats.degenerateTriangleCount;
            referenced[a] = referenced[b] = referenced[c] = 1;
            continue;
        }
        const double* pa = positions.data () + 3 * a;
        const double* pb = positions.data () + 3 * b;
        const double* pc = positions.data () + 3 * c;
        if (geomsrv::IsRoundoffZeroTriangle (pa, pb, pc)) {
            ++stats.degenerateTriangleCount;
            referenced[a] = referenced[b] = referenced[c] = 1;
            continue;
        }
        std::array<uint32_t, 3> key = { a, b, c };
        std::sort (key.begin (), key.end ());
        auto& slots = classes[key]; // value-initialised to {0, 0}
        // Orientation slot: does (a,b,c) appear in even (+1) or odd (-1) form
        // in the sorted triple?
        const bool even = (key[0] == a && key[1] == b) || (key[0] == b && key[1] == c) ||
                          (key[0] == c && key[1] == a);
        ++(even ? slots[0] : slots[1]);
        referenced[a] = referenced[b] = referenced[c] = 1;
    }
    for (const auto& [key, slots] : classes) {
        stats.duplicateTriangleCount += static_cast<size_t> (slots[0] > 0 ? slots[0] - 1 : 0) +
                                        static_cast<size_t> (slots[1] > 0 ? slots[1] - 1 : 0);
        if (slots[0] > 0 && slots[1] > 0)
            stats.interiorTriangleCount += slots[0] + slots[1];
    }

    for (uint8_t used : referenced)
        if (!used)
            ++stats.unreferencedVertexCount;

    MeshTopology topo;
    if (!BuildMeshTopology (positions, triangles, topo, error))
        return false;
    stats.vertexCount = topo.vertexCount;
    stats.triangleCount = topo.triangleCount;
    stats.edgeCount = topo.edgeCount;
    stats.boundaryEdgeCount = topo.boundaryEdgeCount;
    stats.boundaryLoopCount = topo.boundaryLoops.size ();
    stats.nonManifoldEdgeCount = topo.nonManifoldEdgeCount;
    stats.eulerCharacteristic = topo.eulerCharacteristic;
    stats.genus = topo.genus;
    out = std::move (stats);
    return true;
}

} // namespace tapioca::geometry
