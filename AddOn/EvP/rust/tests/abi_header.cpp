#include "GeometryRustFfi.h"

static_assert (sizeof (TapiocaStatus) == 4, "C ABI status must be fixed-width");

extern "C" int TapiocaCppHeaderCheck (void)
{
    uint32_t version = 0;
    return tapioca_version (&version, nullptr, 0) == TAPIOCA_OK && version == TAPIOCA_ABI_VERSION ? 0 : 1;
}
