#ifndef TAPIOCA_GEOMETRY_RUST_FFI_H
#define TAPIOCA_GEOMETRY_RUST_FFI_H

#include <stddef.h>
#include <stdint.h>

/* Aggregate ABI v1. Only implemented exports are declared here. No SDK or
 * Rust/C++ containers, native bool, exceptions, callbacks or retained borrows.
 * The validators below are the legacy geometry-only ingress gate, NOT D14
 * authored-identity validation. Attributed views remain a separate S1 gate. */
#define TAPIOCA_ABI_VERSION UINT32_C (1)

/* Fixed-width status representation shared by future geometry families. */
typedef int32_t TapiocaStatus;
enum { TAPIOCA_OK = 0, TAPIOCA_ERR_ARG = 1, TAPIOCA_ERR_STATE = 2, TAPIOCA_ERR_INTERNAL = 3 };

/* Counts are scalar elements, not xyz/triangle tuple counts. Empty arrays may
 * use NULL. Text is length-delimited UTF-8 bytes, not NUL-terminated. All views
 * are immutable call-scoped borrows; no array is copied or retained. */
typedef struct {
    const char* data;
    size_t count;
} TapiocaTextView;
typedef struct {
    const double* data;
    size_t count;
} TapiocaDoubleView;
typedef struct {
    const float* data;
    size_t count;
} TapiocaFloatView;
typedef struct {
    const uint32_t* data;
    size_t count;
} TapiocaIndexView;
typedef struct {
    const int32_t* data;
    size_t count;
} TapiocaIntView;
typedef struct {
    const uint8_t* data;
    size_t count;
} TapiocaByteView;

typedef struct {
    TapiocaTextView guid; /* Synthetic/empty GUIDs allowed in legacy geometry. */
    int32_t elemType;
    TapiocaDoubleView vertices; /* xyz f64, world metres, right-handed Z-up. */
    TapiocaFloatView normals;   /* Optional xyz, same scalar count as vertices. */
    TapiocaIndexView triangles; /* Triples with indices < vertices.count / 3. */
    TapiocaIntView triMaterial; /* Optional one-per-triangle sidecars. */
    TapiocaByteView triWireEdges;
    TapiocaIntView triSourceBody; /* Paired 1-based Modeler indices, NOT authored IDs. */
    TapiocaIntView triSourcePgon;
    TapiocaIntView triSurfaceAttribute;
} TapiocaMeshView;

typedef struct {
    const TapiocaMeshView* data;
    size_t count;
} TapiocaMeshArrayView;
typedef struct {
    uint64_t id;
    uint64_t captureStamp;
    TapiocaTextView scope;
    uint8_t completeModel; /* 0 or 1 only. */
    TapiocaMeshArrayView meshes;
} TapiocaSnapshotView;

#ifdef __cplusplus
extern "C" {
#endif

/* Pure synchronous version read; no ACAPI or geometry work.
 * version: one aligned writable uint32_t; written only on success.
 * error: caller-owned writable UTF-8 buffer, NUL-terminated when capacity > 0;
 *        success clears it, failure truncates at a UTF-8 boundary. NULL/0 may
 *        discard the message. NULL with nonzero capacity returns ERR_ARG.
 * Buffers must be live for the call, exclusive and nonoverlapping. A detected
 * overlap returns ERR_ARG without writing either buffer. Other detected
 * malformed arguments return ERR_ARG, but checks cannot prove foreign memory
 * is valid. No pointer is retained and no allocation crosses this boundary.
 * Unwinding Rust panics return ERR_INTERNAL; abort/OOM/invalid memory and panic
 * hooks that abort are not recoverable. Caught panic payloads are intentionally
 * leaked to prevent a payload destructor unwinding through the C ABI. */
TapiocaStatus tapioca_version (uint32_t* version, char* error, size_t errorCapacity);

/* Shared legacy geometry ingress, not terrain acceptance or provenance proof.
 * Checks array alignment/length, finite coordinates/normals, index ranges and
 * optional sidecar shape. Empty geometry is valid; no automatic repair or
 * blanket degeneracy/manifold/closure/winding requirement. Modeler source
 * body/polygon sidecars must be both absent or both full with positive indices.
 * Snapshot validation covers meshes/duplicate nonempty GUIDs, not material
 * catalogs, which remain C++ owned and unchanged in this increment.
 * Each input allocation must be live, initialized, aligned and immutable for
 * the call, and nonoverlapping with error storage. Malformed shapes return
 * ERR_ARG with a stable diagnostic-name prefix; duplicate GUIDs return ERR_STATE.
 * Panic and error-buffer discipline match tapioca_version; no input is mutated.
 * Whole-mesh validation is linear work (snapshot GUID checks add ordering work):
 * use workers for unbounded/bulk inputs; synchronous use needs measured budgets. */
TapiocaStatus tapioca_validate_mesh (const TapiocaMeshView* mesh, char* error, size_t errorCapacity);
TapiocaStatus tapioca_validate_snapshot (const TapiocaSnapshotView* snapshot, char* error, size_t errorCapacity);

#ifdef __cplusplus
}
#endif

#endif
