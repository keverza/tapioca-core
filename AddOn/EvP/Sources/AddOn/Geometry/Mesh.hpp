#ifndef GEOMETRYSERVER_MESH_HPP
#define GEOMETRYSERVER_MESH_HPP

#include "Aabb.hpp"

#include <vector>
#include <string>
#include <cstdint>

// Plain geometry containers — no ACAPI, no msgpack. Built on the main thread by
// GeometryExtractor, read on worker threads via MeshStore. All coordinates are
// world-space meters.
namespace geomsrv {

struct Mesh {
    std::string           guid;         // element GUID (Archicad string form)
    int32_t               elemType = 0; // ModelerAPI::Element::Type
    std::vector<double>   vertices;     // xyz interleaved
    std::vector<float>    normals;      // per-vertex xyz (smooth), same count as vertices
    std::vector<uint32_t> triangles;    // 3 vertex indices per triangle
    std::vector<int32_t>  triMaterial;  // one material index per triangle
    Aabb                  bounds;       // element AABB (server-side spatial index)

    size_t VertexCount ()   const { return vertices.size () / 3; }
    size_t TriangleCount () const { return triangles.size () / 3; }

    // Retained heap bytes — so memory held is reportable, not folklore.
    size_t Bytes () const
    {
        return guid.capacity ()
             + vertices.capacity ()    * sizeof (double)
             + normals.capacity ()     * sizeof (float)
             + triangles.capacity ()   * sizeof (uint32_t)
             + triMaterial.capacity () * sizeof (int32_t);
    }
};

struct Snapshot {
    uint64_t          id = 0;
    std::string       scope = "all";   // "all" (whole 3D model) or "selection"
    std::vector<Mesh> meshes;

    size_t TotalTriangles () const
    {
        size_t n = 0;
        for (const auto& m : meshes) n += m.TriangleCount ();
        return n;
    }
    size_t TotalVertices () const
    {
        size_t n = 0;
        for (const auto& m : meshes) n += m.VertexCount ();
        return n;
    }

    // Linear lookup by GUID (snapshots hold tens–hundreds of meshes). Null if absent.
    const Mesh* FindMesh (const std::string& guid) const
    {
        for (const auto& m : meshes)
            if (m.guid == guid) return &m;
        return nullptr;
    }

    size_t Bytes () const
    {
        size_t n = sizeof (Snapshot) + scope.capacity ();
        for (const auto& m : meshes) n += sizeof (Mesh) + m.Bytes ();
        return n;
    }
};

} // namespace geomsrv

#endif
