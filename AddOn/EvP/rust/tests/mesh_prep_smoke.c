#include "GeometryRustFfi.h"
#include <stddef.h>

/* A real C translation unit consumes every new record/function, not a C++-only
 * header check. Exact x64 layout is also checked independently in Rust tests. */
_Static_assert (offsetof (TapiocaWeldCorner, position) == 8, "corner padding");
_Static_assert (sizeof (TapiocaWeldCorner) == 56, "corner layout");
_Static_assert (sizeof (TapiocaMeshAnalysisSummary) == 56, "summary layout");

int mesh_prep_c_smoke (void)
{
    const TapiocaWeldCorner corners[] = { { 1, { 1, 2, 3 }, { 0, 0, 1 } }, { 1, { 9, 9, 9 }, { 0, 0, 1 } } };
    const uint64_t offsets[] = { 0, 2 };
    const TapiocaWeldRequest weld = { { corners, 2 }, { offsets, 2 } };
    size_t count = 99;
    double positions[6] = { 99, 99, 99, 99, 99, 99 };
    float normals[6] = { 99, 99, 99, 99, 99, 99 };
    uint32_t map[2] = { 99, 99 };
    const TapiocaWeldOutput output = { &count, positions, normals, 6, map, 2 };
    char error[256] = { 0 };
    if (tapioca_weld_corners (&weld, &output, error, sizeof error) != TAPIOCA_OK || count != 1 || positions[0] != 1 ||
        positions[3] != 99 || normals[2] != 1 || map[0] != 0 || map[1] != 0)
        return 1;
    const double vertices[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    const uint32_t triangles[] = { 0, 1, 2 };
    const uint64_t edges[] = { 1 }; /* edge0-1 => barycentric bit2. */
    const TapiocaEdgeRange ranges[] = { { 0, 1 } };
    const TapiocaMeshAnalysisRequest analysis = {
        { vertices, 9 }, { NULL, 0 }, { triangles, 3 }, { edges, 1 }, { ranges, 1 }
    };
    TapiocaMeshAnalysisSummary summary = { 0 };
    float smooth[9] = { 0 };
    uint8_t zero = 99, mask = 99;
    const TapiocaMeshAnalysisOutput target = { &summary, smooth, 9, &zero, &mask, 1 };
    if (tapioca_analyze_mesh (&analysis, &target, error, sizeof error) != TAPIOCA_OK || summary.normalsUsable != 0 ||
        smooth[2] != 1 || zero != 0 || mask != 4)
        return 2;
    return 0;
}
