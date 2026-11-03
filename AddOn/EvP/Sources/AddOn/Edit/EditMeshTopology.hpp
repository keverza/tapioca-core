#ifndef TAPIOCA_EDIT_MESHTOPOLOGY_HPP
#define TAPIOCA_EDIT_MESHTOPOLOGY_HPP

// #139 M0: the derived topology view of an indexed triangle mesh. This is an
// ANALYSIS structure, not an editing kernel: it answers adjacency questions
// (edge/face incidence, boundary loops, manifoldness, genus) for Stats,
// Clean gates and later picking. Triangles that carry no edge topology —
// repeated vertex index or roundoff-zero area (the scaled 8-epsilon test of
// TriangleDegeneracy.hpp) — are excluded and counted, so nothing is silent.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tapioca::geometry {

struct MeshTopology {
    size_t vertexCount = 0;
    size_t triangleCount = 0;       // Non-degenerate triangles only.
    size_t degenerateFaceCount = 0; // Triangles excluded: repeated index or zero area.

    // Undirected edge incidence. An edge is a canonical (min, max) vertex pair.
    size_t edgeCount = 0;
    size_t boundaryEdgeCount = 0;    // Exactly 1 adjacent face: an open rim.
    size_t interiorEdgeCount = 0;    // Exactly 2 adjacent faces.
    size_t nonManifoldEdgeCount = 0; // 3+ adjacent faces.

    // Boundary loops as ordered vertex chains; each chain connects back to its
    // first vertex. Traced deterministically from the lowest unvisited
    // half-edge. Half-edges that cannot be traced into a closed loop (only
    // possible on non-manifold rims) stay counted here, not in a loop.
    std::vector<std::vector<uint32_t>> boundaryLoops;
    size_t untracedBoundaryHalfEdges = 0;

    // Euler characteristic chi = V - E + F over the non-degenerate complex,
    // and the orientable genus g = (2 - chi - b) / 2 with b boundary loops.
    // Genus is defined only for a manifold complex (no non-manifold edges);
    // it is -1 there so no number is ever invented for a non-manifold mesh.
    int64_t eulerCharacteristic = 0;
    int32_t genus = -1;

    bool IsManifold () const { return nonManifoldEdgeCount == 0; }
    bool IsClosed () const { return IsManifold () && boundaryEdgeCount == 0; }
};

// Builds the topology view. Fails closed on malformed input (position count
// not a multiple of 3, index count not a multiple of 3, index out of range):
// `out` is left untouched and `error` carries the reason.
bool BuildMeshTopology (std::span<const double> positions,
                        std::span<const uint32_t> triangles,
                        MeshTopology& out,
                        std::string& error);

inline bool BuildMeshTopology (const std::vector<double>& positions,
                               const std::vector<uint32_t>& triangles,
                               MeshTopology& out,
                               std::string& error)
{
    return BuildMeshTopology (std::span (positions), std::span (triangles), out, error);
}

} // namespace tapioca::geometry

#endif
