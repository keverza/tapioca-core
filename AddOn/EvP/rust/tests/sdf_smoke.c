#include "GeometryRustFfi.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

_Static_assert (sizeof (TapiocaSdfCancelCallback) == 16, "cancel layout");
_Static_assert (sizeof (TapiocaSdfEntry) == 216, "entry layout");
_Static_assert (sizeof (TapiocaSdfRequest) == 96, "request layout");
_Static_assert (sizeof (TapiocaSdfCounts) == 32, "counts layout");
_Static_assert (sizeof (TapiocaSdfSummary) == 120, "summary layout");
_Static_assert (sizeof (TapiocaSdfOutput) == 88, "output layout");

int sdf_c_smoke (void)
{
    TapiocaSdfEntry entry;
    memset (&entry, 0, sizeof entry);
    entry.id = 1;
    entry.kind = 2;
    entry.operation = 1;
    entry.profile = 1;
    entry.enabled = 1;
    entry.materialId = 9;
    entry.radius = 1.0;
    const TapiocaSdfRequest request = { { &entry, 1 },
                                        { NULL, 0 },
                                        { NULL, NULL },
                                        0.25,
                                        0,
                                        64000000,
                                        8000000,
                                        2000000,
                                        2,
                                        0 };
    TapiocaSdfCounts counts = { 0 };
    TapiocaSdfSummary summary;
    memset (&summary, 0, sizeof summary);
    char error[256] = { 0 };
    const TapiocaSdfOutput query = { &counts, &summary, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 1, 0 };
    if (tapioca_sdf_build (&request, &query, error, sizeof error) != TAPIOCA_OK || counts.positions == 0 ||
        counts.positions % 3 != 0 || counts.normals != counts.positions || counts.triangles % 3 != 0 ||
        counts.materials != counts.triangles / 3 || summary.contentHash == 0)
        return 1;

    double* positions = (double*) malloc (counts.positions * sizeof *positions);
    float* normals = (float*) malloc (counts.normals * sizeof *normals);
    uint32_t* triangles = (uint32_t*) malloc (counts.triangles * sizeof *triangles);
    int32_t* materials = (int32_t*) malloc (counts.materials * sizeof *materials);
    if (positions == NULL || normals == NULL || triangles == NULL || materials == NULL) {
        free (positions);
        free (normals);
        free (triangles);
        free (materials);
        return 2;
    }
    const uint64_t expected_hash = summary.contentHash;
    const TapiocaSdfOutput fill = { &counts,
                                    &summary,
                                    positions,
                                    counts.positions,
                                    normals,
                                    counts.normals,
                                    triangles,
                                    counts.triangles,
                                    materials,
                                    counts.materials,
                                    0,
                                    0 };
    if (tapioca_sdf_build (&request, &fill, error, sizeof error) != TAPIOCA_OK ||
        summary.contentHash != expected_hash || materials[0] != 9) {
        free (positions);
        free (normals);
        free (triangles);
        free (materials);
        return 3;
    }
    free (positions);
    free (normals);
    free (triangles);
    free (materials);

    entry.operation = 2;
    counts.positions = 777;
    summary.contentHash = 888;
    if (tapioca_sdf_build (&request, &query, error, sizeof error) != TAPIOCA_ERR_ARG || counts.positions != 777 ||
        summary.contentHash != 888 || strncmp (error, "FirstOperationNotUnion:", 23) != 0)
        return 4;
    return 0;
}
