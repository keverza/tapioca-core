#ifndef TAPIOCA_EDIT_PUBLISHEDMESH_HPP
#define TAPIOCA_EDIT_PUBLISHEDMESH_HPP

// #139 M0: the common immutable triangle-mesh snapshot for the derived editor
// lane. Terrain (#112), editable meshes (#139) and volumetric details (#140)
// all publish through this contract instead of lane-specific result types.
// It is deliberately NOT a document: there are no authored IDs and no editing
// state here. Identity is the durable object/family pair plus the definition
// revision/hash the mesh was derived from; tessellation, cleaned topology and
// later LOD indices are derived data and carry remaps to the full-resolution
// mesh, never durable identity of their own (PLAN-LandscapeEditor-112-139-140,
// "Persistent definitions versus evaluated geometry").
//
// Publication goes through EditorMeshStore (EditMeshStore.hpp) — a separate
// derived editor/layer store. Nothing here may overwrite
// geomsrv::MeshStore::current/shared, which stays the native-capture authority.

#include "Geometry/Aabb.hpp"
#include "Geometry/Mesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tapioca::geometry {

struct MeshDiagnostic {
    std::string code;    // Stable machine-readable code, e.g. "index-out-of-range".
    std::string message; // Human-readable detail; never parsed.
    bool operator== (const MeshDiagnostic&) const = default;
};

struct PublishedMesh {
    // Identity. `object` is the durable object id in the editor's own identity
    // space (family prefix + object), NOT a geometric fingerprint.
    std::string object;
    uint64_t family = 0;
    uint64_t revision = 0;           // Definition revision this snapshot answers to.
    uint64_t definitionHash = 0;     // Content hash of the definition it was derived from.
    uint64_t sourceAnchorRevision = 0; // Terrain anchor revision; 0 = not terrain-anchored.

    // Geometry: world-metre f64 positions, indexed triangles. Normals are
    // per-vertex and either match the vertex cardinality exactly or are empty
    // (consumers fall back to geometric face normals, as QueryEngine does).
    std::vector<double> positions;   // xyz interleaved
    std::vector<float> normals;      // per-vertex xyz, or empty
    std::vector<uint32_t> triangles; // 3 vertex indices per triangle
    std::vector<int32_t> triMaterial; // one material index per triangle
    geomsrv::Aabb bounds;            // Recomputed at construction, never trusted from input.

    // Full-resolution provenance/remaps: for every published vertex/triangle
    // the index in the full-resolution mesh this entry derives from. Identity
    // ordering for a full-resolution publish; a cleaned/decimated/LOD mesh maps
    // back to the full-resolution indices through these arrays.
    std::vector<uint32_t> vertexSource;
    std::vector<uint32_t> triangleSource;

    // Health. `valid` is a completeness gate, not a geometry verdict: an
    // invalid mesh must be treated as unusable rather than guessed at.
    bool valid = false;
    std::vector<MeshDiagnostic> diagnostics;

    size_t VertexCount () const { return positions.size () / 3; }
    size_t TriangleCount () const { return triangles.size () / 3; }

    // Retained heap bytes — so memory held is reportable, not folklore.
    size_t Bytes () const
    {
        return object.capacity () + positions.capacity () * sizeof (double) +
               normals.capacity () * sizeof (float) + triangles.capacity () * sizeof (uint32_t) +
               triMaterial.capacity () * sizeof (int32_t) + vertexSource.capacity () * sizeof (uint32_t) +
               triangleSource.capacity () * sizeof (uint32_t) +
               diagnostics.capacity () * sizeof (MeshDiagnostic);
    }
    bool operator== (const PublishedMesh&) const = default;
};

// Deterministic content hash (FNV-1a over a canonical encoding) of the
// GEOMETRY ONLY: positions, normals, triangles, per-triangle materials and the
// source remaps. Identity fields (object/family/revision) and health are
// excluded so two objects sharing geometry share a content hash.
uint64_t HashPublishedMesh (const PublishedMesh& mesh);

// Both conversions fail closed: malformed input (cardinality mismatch,
// out-of-range or non-finite geometry) leaves `out` untouched, sets no guess,
// and reports a diagnostic reason in `error`.
bool ToGeomsrvMesh (const PublishedMesh& mesh, geomsrv::Mesh& out, std::string& error);
bool FromGeomsrvMesh (const geomsrv::Mesh& mesh, PublishedMesh& out, std::string& error);

} // namespace tapioca::geometry

#endif
