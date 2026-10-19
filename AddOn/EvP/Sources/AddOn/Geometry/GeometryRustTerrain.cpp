#include "Geometry/GeometryRustTerrain.hpp"

#include "GeometryRustFfi.h"
#include "Mesh.hpp"

#include <array>
#include <vector>

namespace tapioca::geometry {
namespace {

constexpr size_t kErrorCapacity = 256;
using ErrorBuffer = std::array<char, kErrorCapacity>;

size_t ErrorLength (const ErrorBuffer& buffer)
{
    for (size_t index = 0; index < buffer.size (); ++index) {
        if (buffer[index] == '\0')
            return index;
    }
    return buffer.size ();
}

std::string FailureMessage (TapiocaStatus status, const ErrorBuffer& buffer)
{
    const size_t length = ErrorLength (buffer);
    if (length != 0)
        return std::string (buffer.data (), length);
    // A failed status with no diagnostic is still a failure; never turn it into
    // success or drop the status. This also covers overlap/argument cases where
    // the boundary intentionally writes no message.
    return "Tapioca Rust ABI call failed with status " + std::to_string (status);
}

TapiocaMeshView MakeMeshView (const geomsrv::Mesh& mesh)
{
    TapiocaMeshView view {};
    view.guid = TapiocaTextView { mesh.guid.data (), mesh.guid.size () };
    view.elemType = mesh.elemType;
    view.vertices = TapiocaDoubleView { mesh.vertices.data (), mesh.vertices.size () };
    view.normals = TapiocaFloatView { mesh.normals.data (), mesh.normals.size () };
    view.triangles = TapiocaIndexView { mesh.triangles.data (), mesh.triangles.size () };
    view.triMaterial = TapiocaIntView { mesh.triMaterial.data (), mesh.triMaterial.size () };
    view.triWireEdges = TapiocaByteView { mesh.triWireEdges.data (), mesh.triWireEdges.size () };
    view.triSourceBody = TapiocaIntView { mesh.triSourceBody.data (), mesh.triSourceBody.size () };
    view.triSourcePgon = TapiocaIntView { mesh.triSourcePgon.data (), mesh.triSourcePgon.size () };
    view.triSurfaceAttribute = TapiocaIntView { mesh.triSurfaceAttribute.data (), mesh.triSurfaceAttribute.size () };
    return view;
}

} // namespace

bool ReadRustAbiVersion (uint32_t& outVersion, std::string& error)
{
    ErrorBuffer buffer {};
    uint32_t version = 0;
    const TapiocaStatus status = tapioca_version (&version, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    if (version != TAPIOCA_ABI_VERSION) {
        error = "Tapioca Rust ABI version " + std::to_string (version) + " does not match expected " +
                std::to_string (TAPIOCA_ABI_VERSION);
        return false;
    }
    outVersion = version;
    error.clear ();
    return true;
}

bool ValidateMesh (const geomsrv::Mesh& mesh, std::string& error)
{
    const TapiocaMeshView view = MakeMeshView (mesh);
    ErrorBuffer buffer {};
    const TapiocaStatus status = tapioca_validate_mesh (&view, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    error.clear ();
    return true;
}

bool ValidateSnapshot (const geomsrv::Snapshot& snapshot, std::string& error)
{
    // Own only the descriptors, never the geometry arrays they point at. Catalogs
    // in the geomsrv::Snapshot are deliberately not forwarded or touched.
    std::vector<TapiocaMeshView> descriptors;
    descriptors.reserve (snapshot.meshes.size ());
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        descriptors.push_back (MakeMeshView (mesh));

    TapiocaSnapshotView view {};
    view.id = snapshot.id;
    view.captureStamp = snapshot.captureStamp;
    view.scope = TapiocaTextView { snapshot.scope.data (), snapshot.scope.size () };
    view.completeModel = snapshot.completeModel ? 1 : 0;
    view.meshes = TapiocaMeshArrayView { descriptors.data (), descriptors.size () };

    ErrorBuffer buffer {};
    const TapiocaStatus status = tapioca_validate_snapshot (&view, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
