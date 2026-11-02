#include "GeometryRustFfi.h"
#include <stddef.h>
#include <string.h>

/* A real C translation unit consumes every unified-build record and the
 * export, not a C++-only header check. Exact x64 layout is also checked
 * independently in the Rust tests. */
_Static_assert (sizeof (TapiocaUnifiedPart) == 8, "part layout");
_Static_assert (sizeof (TapiocaUnifiedEntry) == 120, "entry layout");
_Static_assert (sizeof (TapiocaUnifiedPath) == 56, "path layout");
_Static_assert (sizeof (TapiocaUnifiedMask) == 32, "mask layout");
_Static_assert (sizeof (TapiocaUnifiedControl) == 56, "control layout");
_Static_assert (sizeof (TapiocaUnifiedCounts) == 40, "counts layout");
_Static_assert (sizeof (TapiocaUnifiedMeta) == 16, "meta layout");
_Static_assert (sizeof (TapiocaUnifiedLine) == 32, "line layout");
_Static_assert (sizeof (TapiocaUnifiedLocked) == 16, "locked layout");
_Static_assert (offsetof (TapiocaUnifiedRequest, provenance) == 232, "request padding");

int unified_c_smoke (void)
{
    /* 4 m square baseline, two triangles, z = 0. */
    const double points[] = { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    const uint32_t triangles[] = { 0, 1, 2, 0, 2, 3 };
    const double property[] = { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    const double geometry[] = { 1, 1, 0, 3, 1, 0, 3, 3, 0, 1, 3, 0 };
    const TapiocaTerrainRing rings[] = { { 0, 12 } };
    const TapiocaUnifiedPart parts[] = { { 0, 1 } };
    const uint32_t part_indices[] = { 0 };
    const uint64_t sources[] = { 0x1001 };
    const TapiocaTerrainSlope slope = { 100, 0, 2, 0 };
    const TapiocaUnifiedEntry entries[] = { {
        0x101,
        0x202,
        2.0, /* level */
        0.0, /* falloff */
        slope,
        slope,
        0.0, /* reach: sharp daylight target */
        0,
        2,
        1,
        2,
        0,
        1,
        0,
        1,
    } };
    /* Provenance: 4 mesh + 4 property + 4 entry-ring vertices, 4+4 edges,
     * 2 faces, domains=7. */
    TapiocaMappingRecord mappings[22];
    memset (mappings, 0, sizeof mappings);
    size_t at = 0;
    for (uint32_t domain = 1; domain <= 3; ++domain) {
        const uint64_t count = domain == 1 ? 12 : domain == 2 ? 8 : 2;
        for (uint64_t index = 0; index < count; ++index) {
            mappings[at].domain = domain;
            mappings[at].kind = TAPIOCA_MAPPING_GENERATED;
            mappings[at].outputIndex = index;
            mappings[at].sources.offset = 0;
            mappings[at].sources.count = 1;
            mappings[at].operations.offset = 0;
            mappings[at].operations.count = 1;
            mappings[at].object = 0x303;
            ++at;
        }
    }
    const TapiocaSourceRecord source_records[] = { { 0x1001, { 0, 0 } } };
    const TapiocaOperationRecord operation_records[] = { { 0x202, { 0, 0 } } };
    const TapiocaObjectRecord object_records[] = { { 0x303, 0x202 } };
    const uint64_t source_ids[] = { 0x1001 };
    const uint64_t operation_ids[] = { 0x202 };
    uint8_t session[16] = { 1 };
    TapiocaIdentityContext context = { 1, { 0 }, 0, 100 };
    memcpy (context.sessionUuid, session, 16);
    const TapiocaProvenanceRequest provenance = { { 1 },
                                                  0,
                                                  { 12, 8, 2, 0 },
                                                  7,
                                                  1,
                                                  { context,
                                                    { source_records, 1 },
                                                    { operation_records, 1 },
                                                    { object_records, 1 },
                                                    { NULL, 0 },
                                                    { NULL, 0 },
                                                    { source_ids, 1 },
                                                    { operation_ids, 1 },
                                                    { NULL, 0 },
                                                    { mappings, 22 },
                                                    { NULL, 0 },
                                                    { NULL, 0 } } };
    const TapiocaUnifiedRequest request = { { points, 12 },
                                            { triangles, 6 },
                                            { property, 12 },
                                            { geometry, 12 },
                                            { rings, 1 },
                                            { parts, 1 },
                                            { part_indices, 1 },
                                            { NULL, 0 },
                                            { sources, 1 },
                                            { entries, 1 },
                                            { NULL, 0 },
                                            { NULL, 0 },
                                            { NULL, 0 },
                                            1e-3,
                                            7,
                                            0xABCD,
                                            provenance };
    TapiocaUnifiedCounts counts = { 0 };
    TapiocaUnifiedMeta meta = { 0 };
    TapiocaTerrainHybridVolumes site = { 0 }, context_volumes = { 0 };
    char error[256] = { 0 };
    const TapiocaUnifiedOutput query = { &counts, &meta, &site, &context_volumes, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                                         NULL, 0, NULL, 0, NULL, 0, 1, 0 };
    if (tapioca_terrain_unified_build (&request, &query, error, sizeof error) != TAPIOCA_OK ||
        counts.points == 0 || counts.points % 3 != 0 || counts.triangles % 3 != 0 || counts.lines != 4)
        return 1;
    {
        const TapiocaUnifiedCounts expected = counts;
        double out_points[96] = { 0 };
        double base[32] = { 0 };
        uint32_t owners[32] = { 0 };
        uint32_t out_triangles[64] = { 0 };
        TapiocaTerrainMapping out_mappings[96] = { { 0 } };
        TapiocaUnifiedLine out_lines[8] = { { 0 } };
        TapiocaUnifiedLocked out_locked[4] = { { 0 } };
        if (counts.points > 96 || counts.points / 3 > 32 || counts.triangles > 64 || counts.mappings > 96)
            return 2;
        const TapiocaUnifiedOutput fill = { &counts, &meta, &site, &context_volumes,
                                            out_points, counts.points, base, counts.points / 3, owners,
                                            counts.points / 3, out_triangles, counts.triangles, out_mappings,
                                            counts.mappings, out_lines, counts.lines, out_locked, counts.locked, 0, 0 };
        if (tapioca_terrain_unified_build (&request, &fill, error, sizeof error) != TAPIOCA_OK ||
            counts.points != expected.points || counts.lines != expected.lines)
            return 3;
        if (meta.revision != 7 || meta.planHash != 0xABCD || site.fill <= 0.0)
            return 4;
        /* The graded ring's corner (1,1) exists at the sharp target height. */
        int found = 0;
        for (size_t index = 0; index + 2 < counts.points; index += 3)
            found |= out_points[index] == 1.0 && out_points[index + 1] == 1.0 && out_points[index + 2] == 2.0;
        if (!found)
            return 5;
        for (size_t index = 0; index < counts.lines; ++index)
            if (out_lines[index].domain != 1 || out_lines[index].index != 0)
                return 6;
    }
    /* Unknown surface kind refuses without writing anything. */
    {
        TapiocaUnifiedEntry broken_entries[1];
        memcpy (broken_entries, entries, sizeof entries);
        broken_entries[0].surface = 9;
        TapiocaUnifiedRequest bad = request;
        bad.entries.data = broken_entries;
        bad.entries.count = 1;
        counts.points = 99;
        if (tapioca_terrain_unified_build (&bad, &query, error, sizeof error) != TAPIOCA_ERR_ARG ||
            counts.points != 99)
            return 7;
    }
    return 0;
}
