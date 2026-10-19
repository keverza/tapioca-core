#ifndef TAPIOCA_GEOMETRY_RUST_FFI_H
#define TAPIOCA_GEOMETRY_RUST_FFI_H

#include <stddef.h>
#include <stdint.h>

/* Aggregate ABI v1. Only implemented exports are declared here. No SDK or
 * Rust/C++ containers, native bool, exceptions, callbacks or retained borrows.
 * Legacy geometry validation and attributed structural validation are separate
 * gates. No validator proves geometric contributor truth or allocates IDs. */
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

/* Authored identity is session-scoped uint64, never a triangle/index identity.
 * POD records contain only all-bit-valid fields; unknown enum values refuse.
 * Ranges index their documented flat pool with checked offset + count. */
enum { TAPIOCA_ID_SOURCE = 1, TAPIOCA_ID_OPERATION = 2, TAPIOCA_ID_OBJECT = 3 };
enum { TAPIOCA_DOMAIN_VERTEX = 1, TAPIOCA_DOMAIN_EDGE = 2, TAPIOCA_DOMAIN_FACE = 3, TAPIOCA_DOMAIN_PATCH = 4 };
enum {
    TAPIOCA_MAPPING_EXACT = 1,
    TAPIOCA_MAPPING_INHERITED = 2,
    TAPIOCA_MAPPING_SPLIT = 3,
    TAPIOCA_MAPPING_MERGED = 4,
    TAPIOCA_MAPPING_GENERATED = 5
};
enum {
    TAPIOCA_DIAGNOSTIC_NONE = 0,
    TAPIOCA_PROVENANCE_UNATTRIBUTABLE = 1,
    TAPIOCA_PROVENANCE_CONFLICT = 2,
    TAPIOCA_PROVENANCE_STALE = 3,
    TAPIOCA_PATCH_KEY_UNSTABLE = 4
};
#define TAPIOCA_MESH_PROVENANCE_DOMAINS UINT32_C (5) /* Vertex + face mask. */

typedef struct {
    uint64_t offset, count;
} TapiocaIdentityRange;
typedef struct {
    uint32_t schemaVersion;
    uint8_t sessionUuid[16]; /* Non-nil, no Archicad GUID reinterpretation. */
    uint64_t revision;       /* Session document revision; zero is valid. */
    uint64_t tableId;        /* Nonzero call-local table ID, not persisted. */
} TapiocaIdentityContext;
typedef struct {
    uint64_t vertices, edges, faces, patches;
} TapiocaEntityCounts;
typedef struct {
    uint64_t id;
    TapiocaIdentityRange bindings;
} TapiocaSourceRecord;
typedef struct {
    uint64_t id;
    TapiocaIdentityRange parents;
} TapiocaOperationRecord;
typedef struct {
    uint64_t id, operation;
} TapiocaObjectRecord;
typedef struct {
    uint32_t logicalKind;
    uint64_t logicalId;
    uint8_t nativeGuid[36];   /* Uppercase dashed non-nil GUID, no braces/NUL. */
    uint8_t metadataUuid[16]; /* All zero when metadataPresent is zero. */
    uint32_t metadataPresent;
    TapiocaIdentityRange role; /* Nonempty UTF-8 into roleBytes; no NUL. */
    uint64_t revision;         /* Session revision, not model-watch epoch. */
} TapiocaBindingRecord;
typedef struct {
    uint32_t domain, kind;
    uint64_t outputIndex;
    TapiocaIdentityRange sources, operations;
    uint64_t object;
    TapiocaIdentityRange predecessors;
    uint64_t patchKey;
} TapiocaMappingRecord;
typedef struct {
    uint64_t tableId;
    uint32_t domain;
    uint64_t entityIndex;
} TapiocaPredecessorRecord;
typedef struct {
    TapiocaIdentityContext context;
    TapiocaEntityCounts counts;
    uint32_t historical; /* 0: current revision; 1: explicit retained input <= current. */
} TapiocaSupportTable;

typedef struct {
    const TapiocaSourceRecord* data;
    size_t count;
} TapiocaSourceArray;
typedef struct {
    const TapiocaOperationRecord* data;
    size_t count;
} TapiocaOperationArray;
typedef struct {
    const TapiocaObjectRecord* data;
    size_t count;
} TapiocaObjectArray;
typedef struct {
    const TapiocaBindingRecord* data;
    size_t count;
} TapiocaBindingArray;
typedef struct {
    const TapiocaMappingRecord* data;
    size_t count;
} TapiocaMappingArray;
typedef struct {
    const TapiocaPredecessorRecord* data;
    size_t count;
} TapiocaPredecessorArray;
typedef struct {
    const TapiocaSupportTable* data;
    size_t count;
} TapiocaSupportArray;
typedef struct {
    const uint64_t* data;
    size_t count;
} TapiocaIdArray;
typedef struct {
    TapiocaIdentityContext context;
    TapiocaSourceArray sources;
    TapiocaOperationArray operations;
    TapiocaObjectArray objects;
    TapiocaBindingArray bindings;
    TapiocaIdArray bindingRefs;  /* Indices into bindings. */
    TapiocaIdArray sourceIds;    /* Sorted unique contributor sets. */
    TapiocaIdArray operationIds; /* Ordered DAG parents or sorted contributor sets. */
    TapiocaPredecessorArray predecessors;
    TapiocaMappingArray mappings;
    TapiocaByteView roleBytes;
    TapiocaSupportArray supportTables;
} TapiocaProvenanceView;
typedef struct {
    uint8_t expectedSession[16]; /* Trusted current session, supplied separately. */
    uint64_t expectedRevision;
    TapiocaEntityCounts counts; /* Expected output-domain sizes. */
    uint32_t domains;           /* Bit (domain - 1), known nonzero bits 0..3. */
    uint32_t requireObject;     /* Byte-independent boolean: 0 or 1. */
    TapiocaProvenanceView table;
} TapiocaProvenanceRequest;

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

/* Structural attribution gate: contexts/revisions, catalogs and acyclic DAG,
 * native bindings, range/reference validity and complete unique domain coverage.
 * Never generates identity, repairs input, certifies contributor geometry, or
 * establishes persisted patch matching/history allocation by itself.
 * diagnostic is required writable uint32 storage, disjoint from ALL inputs and
 * error. On completed validation it receives NONE or a named code above. On a
 * transport/geometry error or caught panic it remains unchanged. Structural
 * refusals return ERR_STATE with diagnostic + UTF-8 message. All foreign-memory
 * and panic caveats of the legacy gate apply; no pointer is retained.
 * A generic caller derives counts from its retained real geometry tables.
 * Bulk validation runs on workers; no main-thread latency guarantee is implied. */
TapiocaStatus tapioca_validate_provenance (const TapiocaProvenanceRequest* request, uint32_t* diagnostic, char* error,
                                           size_t errorCapacity);
/* This ingress declares vertex+face attribution (domains == 5), derives their
 * counts from mesh arrays and refuses mismatches; edge/patch output counts must
 * be zero. Existing Modeler sidecars stay additive and unchanged. */
TapiocaStatus tapioca_validate_attributed_mesh (const TapiocaMeshView* mesh, const TapiocaProvenanceRequest* request,
                                                uint32_t* diagnostic, char* error, size_t errorCapacity);

#ifdef __cplusplus
}
#endif

#endif
