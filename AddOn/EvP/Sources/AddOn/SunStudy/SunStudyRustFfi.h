#ifndef TAPIOCA_SUNSTUDY_RUST_FFI_H
#define TAPIOCA_SUNSTUDY_RUST_FFI_H

/* Sun-study C ABI v1, exported by the aggregate tapioca_core_ffi archive.
 * Only implemented exports are declared here. Shares TapiocaStatus and the
 * error-buffer envelope with GeometryRustFfi.h: every call returns a status,
 * writes a NUL-terminated UTF-8 diagnostic prefix on failure, leaves outputs
 * unchanged on refusal, and contains Rust panics (TAPIOCA_ERR_INTERNAL).
 * No SDK types, containers, native bool, exceptions or retained borrows. */

#include "Geometry/GeometryRustFfi.h"

#define TAPIOCA_SUNSTUDY_ABI_VERSION UINT32_C (1)

#ifdef __cplusplus
extern "C" {
#endif

/* The sun-study surface's own version, independent of TAPIOCA_ABI_VERSION. */
TapiocaStatus tapioca_sunstudy_version (uint32_t* version, char* error, size_t errorCapacity);

#ifdef __cplusplus
}
#endif

#endif
