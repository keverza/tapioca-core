#ifndef TAPIOCA_EDIT_MESHSTATS_HPP
#define TAPIOCA_EDIT_MESHSTATS_HPP

// #139 M0: MeshStats — the inspection report of one indexed triangle mesh.
// Every classification is pinned here so the numbers are reproducible and
// testable, and Clean's report speaks the same vocabulary:
//
//   degenerate — repeated vertex index, or roundoff-zero area (the scaled
//                8-epsilon cross test of TriangleDegeneracy.hpp; small but
//                nonzero triangles are NOT degenerate);
//   duplicate  — a triangle whose three-vertex set coincides with an earlier
//                triangle's set, same cyclic orientation (extras beyond the
//                first are counted);
//   interior   — a triangle in a coincidence class that contains BOTH cyclic
//                orientations: its reversed twin exists in the same mesh, so
//                the pair is a zero-thickness internal wall. All members of
//                such a class count as interior;
//
// All three coincidence tests work on vertex INDICES. Positional coincidence
// is Clean's weld step, which runs before its own duplicate/interior pass.

#include "Edit/EditMeshTopology.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tapioca::geometry {

struct MeshStats {
    // Topology view (non-degenerate complex).
    size_t vertexCount = 0;
    size_t triangleCount = 0;
    size_t edgeCount = 0;
    size_t boundaryEdgeCount = 0;
    size_t boundaryLoopCount = 0;
    size_t nonManifoldEdgeCount = 0;
    int64_t eulerCharacteristic = 0;
    int32_t genus = -1; // -1: undefined because the complex is non-manifold.

    // Health classifications on the raw indexed mesh (before any weld).
    size_t degenerateTriangleCount = 0; // Repeated index or roundoff-zero area.
    size_t duplicateTriangleCount = 0;  // Coincident same-orientation extras.
    size_t interiorTriangleCount = 0;   // Coincidence classes with both orientations.
    size_t unreferencedVertexCount = 0; // Vertices no triangle uses at all.

    bool IsManifold () const { return nonManifoldEdgeCount == 0; }
    bool IsClosed () const { return IsManifold () && boundaryEdgeCount == 0; }
};

// Computes the stats report. Fails closed on malformed input (same gate as
// BuildMeshTopology): `out` is left untouched and `error` carries the reason.
bool ComputeMeshStats (std::span<const double> positions,
                       std::span<const uint32_t> triangles,
                       MeshStats& out,
                       std::string& error);

inline bool ComputeMeshStats (const std::vector<double>& positions,
                              const std::vector<uint32_t>& triangles,
                              MeshStats& out,
                              std::string& error)
{
    return ComputeMeshStats (std::span (positions), std::span (triangles), out, error);
}

} // namespace tapioca::geometry

#endif
