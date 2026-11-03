#include "Edit/EditPublishedMesh.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace tapioca::geometry {
namespace {

void HashBytes (uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*> (data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

// Doubles hash by bit pattern so NaN/Inf positions hash deterministically and
// -0.0 is distinguishable from +0.0 (the same convention as the plan hash in
// EditLandscapeFieldPlan.cpp).
void HashNumber (uint64_t& hash, double value)
{
    uint64_t bits = 0;
    static_assert (sizeof bits == sizeof value);
    std::memcpy (&bits, &value, sizeof bits);
    HashBytes (hash, &bits, sizeof bits);
}

void HashCount (uint64_t& hash, uint64_t count)
{
    HashBytes (hash, &count, sizeof count);
}

} // namespace

uint64_t HashPublishedMesh (const PublishedMesh& mesh)
{
    uint64_t hash = 1469598103934665603ull;
    HashCount (hash, mesh.VertexCount ());
    for (double v : mesh.positions)
        HashNumber (hash, v);
    HashCount (hash, mesh.normals.size ());
    for (float n : mesh.normals)
        HashBytes (hash, &n, sizeof n);
    HashCount (hash, mesh.TriangleCount ());
    HashBytes (hash, mesh.triangles.data (), mesh.triangles.size () * sizeof (uint32_t));
    HashBytes (hash, mesh.triMaterial.data (), mesh.triMaterial.size () * sizeof (int32_t));
    HashBytes (hash, mesh.vertexSource.data (), mesh.vertexSource.size () * sizeof (uint32_t));
    HashBytes (hash, mesh.triangleSource.data (), mesh.triangleSource.size () * sizeof (uint32_t));
    return hash;
}

bool ToGeomsrvMesh (const PublishedMesh& mesh, geomsrv::Mesh& out, std::string& error)
{
    if (mesh.triangles.size () % 3 != 0) {
        error = "triangle index count is not a multiple of 3";
        return false;
    }
    const size_t vertexCount = mesh.VertexCount ();
    if (mesh.positions.size () % 3 != 0) {
        error = "position count is not a multiple of 3";
        return false;
    }
    if (!mesh.normals.empty () && mesh.normals.size () != mesh.positions.size ()) {
        error = "normal cardinality does not match vertex cardinality";
        return false;
    }
    if (!mesh.triMaterial.empty () && mesh.triMaterial.size () != mesh.TriangleCount ()) {
        error = "per-triangle material cardinality does not match triangle count";
        return false;
    }
    for (uint32_t index : mesh.triangles)
        if (index >= vertexCount) {
            error = "triangle index out of range";
            return false;
        }

    geomsrv::Mesh converted;
    converted.guid = mesh.object;
    converted.elemType = static_cast<int32_t> (mesh.family);
    converted.vertices = mesh.positions;
    converted.normals = mesh.normals;
    converted.triangles = mesh.triangles;
    converted.triMaterial =
        mesh.triMaterial.empty () ? std::vector<int32_t> (mesh.TriangleCount (), 0) : mesh.triMaterial;
    converted.bounds = mesh.bounds;
    out = std::move (converted);
    return true;
}

bool FromGeomsrvMesh (const geomsrv::Mesh& mesh, PublishedMesh& out, std::string& error)
{
    if (mesh.vertices.size () % 3 != 0) {
        error = "vertex count is not a multiple of 3";
        return false;
    }
    if (mesh.triangles.size () % 3 != 0) {
        error = "triangle index count is not a multiple of 3";
        return false;
    }
    if (!mesh.normals.empty () && mesh.normals.size () != mesh.vertices.size ()) {
        error = "normal cardinality does not match vertex cardinality";
        return false;
    }
    if (!mesh.triMaterial.empty () && mesh.triMaterial.size () != mesh.TriangleCount ()) {
        error = "per-triangle material cardinality does not match triangle count";
        return false;
    }
    const size_t vertexCount = mesh.VertexCount ();
    for (size_t i = 0; i < mesh.vertices.size (); ++i)
        if (!std::isfinite (mesh.vertices[i])) {
            error = "non-finite position";
            return false;
        }
    for (uint32_t index : mesh.triangles)
        if (index >= vertexCount) {
            error = "triangle index out of range";
            return false;
        }

    PublishedMesh converted;
    converted.object = mesh.guid;
    converted.family = static_cast<uint64_t> (mesh.elemType);
    converted.positions = mesh.vertices;
    converted.normals = mesh.normals;
    converted.triangles = mesh.triangles;
    converted.triMaterial = mesh.triMaterial;
    // Bounds are recomputed, not trusted from the input: a stale or synthetic
    // source AABB must not survive into the published snapshot.
    for (size_t i = 0; i + 2 < converted.positions.size (); i += 3)
        converted.bounds.Expand (converted.positions[i], converted.positions[i + 1], converted.positions[i + 2]);
    const size_t countV = converted.VertexCount ();
    const size_t countT = converted.TriangleCount ();
    converted.vertexSource.resize (countV);
    for (uint32_t i = 0; i < countV; ++i)
        converted.vertexSource[i] = i;
    converted.triangleSource.resize (countT);
    for (uint32_t i = 0; i < countT; ++i)
        converted.triangleSource[i] = i;
    if (converted.triMaterial.empty ())
        converted.triMaterial.assign (countT, 0);
    converted.valid = true;
    out = std::move (converted);
    return true;
}

} // namespace tapioca::geometry
