#include "GeometryRustFfi.h"

#include <stdio.h>
#include <string.h>

int TapiocaCppHeaderCheck (void);

int main (void)
{
    uint32_t version = 99;
    char error[128] = "not cleared";
    if (sizeof (TapiocaStatus) != 4 || tapioca_version (&version, error, sizeof (error)) != TAPIOCA_OK ||
        version != TAPIOCA_ABI_VERSION || error[0] != '\0')
        return 1;

    if (tapioca_version (NULL, error, sizeof (error)) != TAPIOCA_ERR_ARG ||
        strstr (error, "Null or misaligned") == NULL)
        return 2;

    version = 99;
    if (tapioca_version (&version, NULL, 1) != TAPIOCA_ERR_ARG || version != 99)
        return 3;
    if (tapioca_version (&version, NULL, 0) != TAPIOCA_OK || version != TAPIOCA_ABI_VERSION)
        return 4;
    if (TapiocaCppHeaderCheck () != 0)
        return 5;

    if (sizeof (void*) == 8 &&
        (sizeof (TapiocaMeshView) != 152 || sizeof (TapiocaSnapshotView) != 56 ||
         offsetof (TapiocaMeshView, vertices) != 24 || offsetof (TapiocaSnapshotView, meshes) != 40))
        return 6;

    double vertices[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    uint32_t triangles[] = { 0, 1, 2 };
    TapiocaMeshView mesh = { 0 };
    mesh.guid.data = "synthetic";
    mesh.guid.count = 9;
    mesh.vertices.data = vertices;
    mesh.vertices.count = 9;
    mesh.triangles.data = triangles;
    mesh.triangles.count = 3;
    if (tapioca_validate_mesh (&mesh, error, sizeof (error)) != TAPIOCA_OK)
        return 7;
    triangles[2] = 3;
    if (tapioca_validate_mesh (&mesh, error, sizeof (error)) != TAPIOCA_ERR_ARG ||
        strstr (error, "MeshIndexOutOfBounds:") != error)
        return 8;
    triangles[2] = 2;
    TapiocaMeshView meshes[] = { mesh, mesh };
    TapiocaSnapshotView snapshot = { 0 };
    snapshot.meshes.data = meshes;
    snapshot.meshes.count = 2;
    if (tapioca_validate_snapshot (&snapshot, error, sizeof (error)) != TAPIOCA_ERR_STATE ||
        strstr (error, "SnapshotDuplicateGuid:") != error)
        return 9;

    if (sizeof (void*) == 8 &&
        (sizeof (TapiocaIdentityContext) != 40 || sizeof (TapiocaBindingRecord) != 96 ||
         sizeof (TapiocaMappingRecord) != 80 || sizeof (TapiocaProvenanceRequest) != 280 ||
         offsetof (TapiocaProvenanceRequest, table) != 64 || offsetof (TapiocaBindingRecord, role) != 72))
        return 10;
    TapiocaProvenanceRequest request = { 0 };
    request.expectedSession[0] = 1;
    request.table.context.schemaVersion = 1;
    request.table.context.sessionUuid[0] = 1;
    request.table.context.tableId = 100;
    request.domains = TAPIOCA_MESH_PROVENANCE_DOMAINS;
    uint32_t diagnostic = 99;
    if (tapioca_validate_provenance (&request, &diagnostic, error, sizeof (error)) != TAPIOCA_OK || diagnostic != 0)
        return 11;
    request.expectedRevision = 1;
    if (tapioca_validate_provenance (&request, &diagnostic, error, sizeof (error)) != TAPIOCA_ERR_STATE ||
        diagnostic != TAPIOCA_PROVENANCE_STALE || strstr (error, "ProvenanceStale:") != error)
        return 12;

    if (sizeof (void*) == 8 && (sizeof (TapiocaAabb) != 48 || sizeof (TapiocaMeshValueView) != 200 ||
                                sizeof (TapiocaSnapshotValueRequest) != 120))
        return 13;
    TapiocaSnapshotValueRequest valueRequest = { 0 };
    valueRequest.snapshot.scope.data = "all";
    valueRequest.snapshot.scope.count = 3;
    if (tapioca_validate_snapshot_value (&valueRequest, error, sizeof (error)) != TAPIOCA_OK)
        return 14;

    if (sizeof (void*) == 8 && (sizeof (TapiocaAffineTransform) != 96 || sizeof (TapiocaAffineRequest) != 120 ||
                                sizeof (TapiocaAffineOutput) != 24))
        return 15;
    const double point[] = { 1, 2, 3 };
    double transformed[] = { 99, 99, 99 };
    uint32_t flipped = 99;
    TapiocaAffineRequest affine = { 0 };
    affine.transform.m[0][0] = affine.transform.m[1][1] = affine.transform.m[2][2] = 1;
    affine.transform.m[0][3] = 10;
    affine.mode = TAPIOCA_VECTOR_POINT;
    affine.values.data = point;
    affine.values.count = 3;
    TapiocaAffineOutput affineOutput = { transformed, 3, &flipped };
    if (tapioca_transform_vectors (&affine, &affineOutput, error, sizeof (error)) != TAPIOCA_OK ||
        transformed[0] != 11 || transformed[1] != 2 || transformed[2] != 3 || flipped != 0)
        return 16;

    TapiocaAffineTransform composed = { 0 };
    if (tapioca_compose_transforms (&affine.transform, 1, &composed, error, sizeof (error)) != TAPIOCA_OK ||
        composed.m[0][3] != 10 || composed.m[2][2] != 1)
        return 17;
    if (tapioca_compose_transforms (NULL, 0, &composed, error, sizeof (error)) != TAPIOCA_OK || composed.m[0][3] != 0 ||
        composed.m[0][0] != 1)
        return 18;

    if (sizeof (void*) == 8 && (sizeof (TapiocaPolylineRequest) != 32 || sizeof (TapiocaPolylineOutput) != 48 ||
                                offsetof (TapiocaPolylineOutput, totalLength) != 40))
        return 19;
    const double line[] = { 0, 0, 0, 0, 0, 8 }, parameters[] = { 0.5 };
    double cumulative[2], sampled[3], tangents[3], total;
    const TapiocaPolylineRequest polyline = { { line, 6 }, { parameters, 1 } };
    const TapiocaPolylineOutput polylineOutput = { cumulative, 2, sampled, tangents, 3, &total };
    if (tapioca_sample_polyline (&polyline, &polylineOutput, error, sizeof (error)) != TAPIOCA_OK || total != 8 ||
        cumulative[0] != 0 || cumulative[1] != 8 || sampled[2] != 4 || tangents[2] != 1 || error[0] != '\0')
        return 20;

    TapiocaArcRequest arc = { { 0, 0, 0 }, { 0, 0, 1 }, 2, 0, 1, 2 };
    double arcPoints[9] = { 0 };
    if (sizeof (void*) == 8 && (sizeof (TapiocaArcRequest) != 80 || offsetof (TapiocaArcRequest, segments) != 72))
        return 20;
    if (tapioca_make_arc (&arc, arcPoints, 9, error, sizeof (error)) != TAPIOCA_OK || arcPoints[1] != 2)
        return 21;

    TapiocaTransformBuilderRequest builder = { TAPIOCA_BUILD_SCALING, { 10, 20, 30 }, { -2, 3, 0.5 }, 0 };
    TapiocaAffineTransform built = { 0 };
    if (sizeof (void*) == 8 && (sizeof (builder) != 64 || offsetof (TapiocaTransformBuilderRequest, radians) != 56))
        return 22;
    if (tapioca_build_transform (&builder, &built, error, sizeof (error)) != TAPIOCA_OK || built.m[0][0] != -2 ||
        built.m[0][3] != 30 || built.m[1][3] != -40)
        return 23;

    const double planePoints[] = { 1, 2, 9 };
    const TapiocaPlaneRequest plane = { { { 0, 0, 5 }, { 0, 0, 7 }, { 0, 0, 0 } }, { planePoints, 3 } };
    TapiocaPlaneFrame planeFrame = { 0 };
    double projected[3] = { 99, 99, 99 };
    const TapiocaPlaneOutput planeOutput = { &planeFrame, projected, 3 };
    if (sizeof (void*) == 8 &&
        (sizeof (TapiocaPlaneRequest) != 88 || sizeof (TapiocaPlaneFrame) != 120 || sizeof (TapiocaPlaneOutput) != 24))
        return 24;
    if (tapioca_project_to_plane (&plane, &planeOutput, error, sizeof (error)) != TAPIOCA_OK || projected[2] != 5 ||
        planeFrame.plane.normal[2] != 7 || planeFrame.u[0] != 1)
        return 25;

    printf ("Tapioca aggregate ABI v%u: C and C++ link/status checks passed\n", (unsigned) version);
    return 0;
}
