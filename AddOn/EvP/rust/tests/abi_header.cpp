#include "GeometryRustFfi.h"

static_assert (sizeof (TapiocaStatus) == 4, "C ABI status must be fixed-width");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaMeshView) == 152, "Rust/C++ mesh layout mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaMeshView, vertices) == 24,
               "Rust/C++ mesh field offset mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSnapshotView) == 56, "Rust/C++ snapshot layout mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaSnapshotView, meshes) == 40,
               "Rust/C++ snapshot field offset mismatch");

extern "C" int TapiocaCppHeaderCheck (void)
{
    uint32_t version = 0;
    return tapioca_version (&version, nullptr, 0) == TAPIOCA_OK && version == TAPIOCA_ABI_VERSION ? 0 : 1;
}
