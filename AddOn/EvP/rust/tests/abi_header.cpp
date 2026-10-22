#include "GeometryRustFfi.h"

static_assert (sizeof (TapiocaStatus) == 4, "C ABI status must be fixed-width");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaMeshView) == 152, "Rust/C++ mesh layout mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaMeshView, vertices) == 24,
               "Rust/C++ mesh field offset mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSnapshotView) == 56, "Rust/C++ snapshot layout mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaSnapshotView, meshes) == 40,
               "Rust/C++ snapshot field offset mismatch");

static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainRequest) == 368, "Terrain request ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainSplitRequest) == 344, "Terrain split ABI mismatch");
static_assert (sizeof (TapiocaTerrainMapping) == 32, "Terrain mapping ABI mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaTerrainOutput, queryOnly) == 56, "Terrain output ABI mismatch");

extern "C" int TapiocaCppHeaderCheck (void)
{
    uint32_t version = 0;
    return tapioca_version (&version, nullptr, 0) == TAPIOCA_OK && version == TAPIOCA_ABI_VERSION ? 0 : 1;
}
