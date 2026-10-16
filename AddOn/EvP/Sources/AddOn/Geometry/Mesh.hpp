#ifndef GEOMETRYSERVER_MESH_HPP
#define GEOMETRYSERVER_MESH_HPP

#include "Aabb.hpp"

#include <vector>
#include <string>
#include <cstdint>
#include <map>

// Plain geometry containers — no ACAPI, no msgpack. Built on the main thread by
// GeometryExtractor, read on worker threads via MeshStore. All coordinates are
// world-space meters.
namespace geomsrv {

struct Mesh {
    std::string guid;                 // element GUID (Archicad string form)
    int32_t elemType = 0;             // ModelerAPI::Element::Type
    std::vector<double> vertices;     // xyz interleaved
    std::vector<float> normals;       // per-vertex xyz (smooth), same count as vertices
    std::vector<uint32_t> triangles;  // 3 vertex indices per triangle
    std::vector<int32_t> triMaterial; // one material index per triangle
    // Three source-face boundary bits per triangle. Internal triangulation
    // edges stay clear so the wire pass can omit them without changing geometry.
    std::vector<uint8_t> triWireEdges;
    // Per-triangle provenance, one entry per triangle in `triangles` (the same
    // length as triMaterial once extraction is done):
    //   triSourceBody — the 1-BASED ModelerAPI::Element::GetTessellatedBody
    //                   index the triangle was emitted from;
    //   triSourcePgon — the 1-BASED MeshBody::GetPolygon index inside that body.
    //
    // ⚠️ THESE ARE THE MODELER'S OWN INDICES AND NOTHING ELSE. They are NOT the
    // C 3D-database elem/body/polygon ids of ACAPI_ModelAccess_* and NOT the
    // native picker's dbElemIdx/bodyIdx/ipgon. Nothing may assume the numberings
    // agree; correlate by geometry (Geometry/SurfaceSourceMap). Empty on a mesh
    // built outside the extractor (Primitives), which means "no provenance" and
    // must be treated as unusable rather than guessed at.
    std::vector<int32_t> triSourceBody;
    std::vector<int32_t> triSourcePgon;
    // Original project SURFACE attribute from the polygon's AttributeIndex,
    // not triMaterial's model-pool index. Zero is unknown/generated. Empty on
    // old serialized/synthetic meshes; consumers must never guess a mapping.
    std::vector<int32_t> triSurfaceAttribute;
    Aabb bounds; // element AABB (server-side spatial index)

    size_t VertexCount () const
    {
        return vertices.size () / 3;
    }
    size_t TriangleCount () const
    {
        return triangles.size () / 3;
    }

    // Retained heap bytes — so memory held is reportable, not folklore.
    size_t Bytes () const
    {
        return guid.capacity () + vertices.capacity () * sizeof (double) + normals.capacity () * sizeof (float) +
               triangles.capacity () * sizeof (uint32_t) + triMaterial.capacity () * sizeof (int32_t) +
               triWireEdges.capacity () * sizeof (uint8_t) + triSourceBody.capacity () * sizeof (int32_t) +
               triSourcePgon.capacity () * sizeof (int32_t) + triSurfaceAttribute.capacity () * sizeof (int32_t);
    }
};

struct CapturedSurface {
    std::string name, guid;
    bool operator== (const CapturedSurface&) const = default;
};
struct CapturedMaterialIdentity {
    std::string name;
    int32_t externalReference = 0; // identity evidence only, never a surface index
    bool operator== (const CapturedMaterialIdentity&) const = default;
};
struct Snapshot {
    uint64_t id = 0;
    std::string scope = "all"; // "all" (whole 3D model) or "selection"
    std::vector<Mesh> meshes;
    // Modeler material indices belong to this exact capture's pool. Keeping
    // transparency beside the geometry prevents a later pool renumbering from
    // reclassifying old faces. Fractions, not percentages; missing is unknown.
    std::map<int32_t, double> materialTransparency;
    // Detached catalogs read in the same capture gate as the model acquisition.
    // Pool identity also guards incremental reuse when equal-transparency entries
    // are renumbered. Project indices are validated against the surface catalog.
    std::map<int32_t, CapturedMaterialIdentity> materialIdentity;
    std::map<int32_t, CapturedSurface> surfaceAttributes;
    uint64_t captureStamp = 0;  // nonzero model-watch epoch/revision, not a snapshot id
    bool completeModel = false; // never true for selection/type-filtered/partial walks

    size_t TotalTriangles () const
    {
        size_t n = 0;
        for (const auto& m : meshes)
            n += m.TriangleCount ();
        return n;
    }
    size_t TotalVertices () const
    {
        size_t n = 0;
        for (const auto& m : meshes)
            n += m.VertexCount ();
        return n;
    }

    // Linear lookup by GUID (snapshots hold tens–hundreds of meshes). Null if absent.
    const Mesh* FindMesh (const std::string& guid) const
    {
        for (const auto& m : meshes)
            if (m.guid == guid)
                return &m;
        return nullptr;
    }

    size_t Bytes () const
    {
        size_t n = sizeof (Snapshot) + scope.capacity () +
                   materialTransparency.size () * (sizeof (std::pair<const int32_t, double>) + 3 * sizeof (void*));
        for (const auto& m : meshes)
            n += sizeof (Mesh) + m.Bytes ();
        for (const auto& [index, material] : materialIdentity)
            n += sizeof (std::pair<const int32_t, CapturedMaterialIdentity>) + 3 * sizeof (void*) +
                 material.name.capacity ();
        for (const auto& [index, surface] : surfaceAttributes)
            n += sizeof (std::pair<const int32_t, CapturedSurface>) + 3 * sizeof (void*) + surface.name.capacity () +
                 surface.guid.capacity ();
        return n;
    }
};

} // namespace geomsrv

#endif
