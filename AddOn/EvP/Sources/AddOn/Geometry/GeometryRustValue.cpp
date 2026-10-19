#include "Geometry/GeometryRustTerrain.hpp"
#include "GeometryRustFfi.h"
#include "Mesh.hpp"

#include <array>
#include <vector>

namespace tapioca::geometry {
namespace {

using ErrorBuffer = std::array<char, 256>;

bool Outcome (TapiocaStatus status, const ErrorBuffer& buffer, std::string& error)
{
    if (status == TAPIOCA_OK) {
        error.clear ();
        return true;
    }
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    error = length != 0 ? std::string (buffer.data (), length)
                        : "Tapioca Rust value validation failed with status " + std::to_string (status);
    return false;
}

TapiocaMeshValueView MeshValue (const geomsrv::Mesh& mesh)
{
    TapiocaMeshValueView view {};
    view.mesh.guid = { mesh.guid.data (), mesh.guid.size () };
    view.mesh.elemType = mesh.elemType;
    view.mesh.vertices = { mesh.vertices.data (), mesh.vertices.size () };
    view.mesh.normals = { mesh.normals.data (), mesh.normals.size () };
    view.mesh.triangles = { mesh.triangles.data (), mesh.triangles.size () };
    view.mesh.triMaterial = { mesh.triMaterial.data (), mesh.triMaterial.size () };
    view.mesh.triWireEdges = { mesh.triWireEdges.data (), mesh.triWireEdges.size () };
    view.mesh.triSourceBody = { mesh.triSourceBody.data (), mesh.triSourceBody.size () };
    view.mesh.triSourcePgon = { mesh.triSourcePgon.data (), mesh.triSourcePgon.size () };
    view.mesh.triSurfaceAttribute = { mesh.triSurfaceAttribute.data (), mesh.triSurfaceAttribute.size () };
    for (size_t axis = 0; axis < 3; ++axis) {
        view.bounds.mn[axis] = mesh.bounds.mn[axis];
        view.bounds.mx[axis] = mesh.bounds.mx[axis];
    }
    return view;
}

} // namespace

bool ValidateMeshValue (const geomsrv::Mesh& mesh, std::string& error)
{
    const auto view = MeshValue (mesh);
    ErrorBuffer buffer {};
    const auto status = tapioca_validate_mesh_value (&view, buffer.data (), buffer.size ());
    return Outcome (status, buffer, error);
}

bool ValidateSnapshotValue (const geomsrv::Snapshot& snapshot, const SnapshotValueOptions& options, std::string& error)
{
    // Only small descriptors cross into local vectors. All mesh arrays and text
    // stay immutable borrows of the retained C++ snapshot for this entire call.
    std::vector<TapiocaMeshValueView> meshes;
    meshes.reserve (snapshot.meshes.size ());
    for (const auto& mesh : snapshot.meshes)
        meshes.push_back (MeshValue (mesh));
    std::vector<TapiocaMaterialTransparency> transparency;
    transparency.reserve (snapshot.materialTransparency.size ());
    for (const auto& [index, fraction] : snapshot.materialTransparency)
        transparency.push_back ({ index, fraction });
    std::vector<TapiocaMaterialIdentity> materials;
    materials.reserve (snapshot.materialIdentity.size ());
    for (const auto& [index, material] : snapshot.materialIdentity)
        materials.push_back ({ index, material.externalReference, { material.name.data (), material.name.size () } });
    std::vector<TapiocaSurfaceAttribute> surfaces;
    surfaces.reserve (snapshot.surfaceAttributes.size ());
    for (const auto& [index, surface] : snapshot.surfaceAttributes)
        surfaces.push_back (
            { index, { surface.name.data (), surface.name.size () }, { surface.guid.data (), surface.guid.size () } });

    TapiocaSnapshotValueRequest request {};
    request.snapshot.id = snapshot.id;
    request.snapshot.captureStamp = snapshot.captureStamp;
    request.snapshot.scope = { snapshot.scope.data (), snapshot.scope.size () };
    request.snapshot.completeModel = snapshot.completeModel ? 1U : 0U;
    request.snapshot.meshes = { meshes.data (), meshes.size () };
    request.snapshot.materialTransparency = { transparency.data (), transparency.size () };
    request.snapshot.materialIdentity = { materials.data (), materials.size () };
    request.snapshot.surfaceAttributes = { surfaces.data (), surfaces.size () };
    request.expectedCaptureStamp = options.expectedCaptureStamp;
    request.requireCatalogs = options.requireCatalogs ? 1U : 0U;
    ErrorBuffer buffer {};
    const auto status = tapioca_validate_snapshot_value (&request, buffer.data (), buffer.size ());
    return Outcome (status, buffer, error);
}

} // namespace tapioca::geometry
