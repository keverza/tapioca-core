#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"
#include "Geometry/Mesh.hpp"

#include <iostream>

int main ()
{
    using namespace tapioca::geometry;
    static_assert (sizeof (TapiocaAabb) == 48);
    static_assert (sizeof (TapiocaMeshValueView) == 200);
    static_assert (sizeof (TapiocaMaterialTransparency) == 16);
    static_assert (sizeof (TapiocaMaterialIdentity) == 24);
    static_assert (sizeof (TapiocaSurfaceAttribute) == 40);
    static_assert (sizeof (TapiocaSnapshotValueView) == 104);
    static_assert (sizeof (TapiocaSnapshotValueRequest) == 120);
    geomsrv::Snapshot snapshot;
    std::string error;
    if (!ValidateSnapshotValue (snapshot, {}, error))
        return 1;
    snapshot.id = 42;
    snapshot.captureStamp = 7;
    snapshot.completeModel = true;
    geomsrv::Mesh mesh;
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh.triangles = { 0, 1, 2 };
    mesh.triMaterial = { 1 };
    mesh.triSurfaceAttribute = { 42 };
    mesh.bounds.Expand (0, 0, 0);
    mesh.bounds.Expand (1, 1, 0);
    snapshot.meshes.push_back (mesh);
    snapshot.materialTransparency[1] = 0.5;
    snapshot.materialIdentity[1] = { "material", 42 };
    snapshot.surfaceAttributes[42] = { "surface", "12345678-ABCD-1234-ABCD-123456789ABC" };
    const auto before = snapshot;
    if (!ValidateSnapshotValue (snapshot, { 7, true }, error) || !error.empty ())
        return 2;
    if (ValidateSnapshotValue (snapshot, { 8, true }, error) || error.empty ())
        return 3;
    if (snapshot.materialIdentity != before.materialIdentity ||
        snapshot.surfaceAttributes != before.surfaceAttributes ||
        snapshot.meshes.front ().vertices != before.meshes.front ().vertices)
        return 4;
    snapshot.materialIdentity.clear ();
    if (!ValidateSnapshotValue (snapshot, {}, error) || ValidateSnapshotValue (snapshot, { 7, true }, error))
        return 5;
    mesh.bounds.mx[1] = -1;
    if (ValidateMeshValue (mesh, error) || error.empty ())
        return 6;
    std::cout << "Tapioca detached value/catalog validation passed\n";
    return 0;
}
