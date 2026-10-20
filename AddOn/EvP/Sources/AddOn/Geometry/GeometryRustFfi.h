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

/* Complete detached value views. Legacy records above keep their limited
 * geometry-only semantics and layout. Bounds/catalogs are opt-in here. */
typedef struct {
    double mn[3], mx[3];
} TapiocaAabb;
typedef struct {
    TapiocaMeshView mesh;
    TapiocaAabb bounds;
} TapiocaMeshValueView;
typedef struct {
    const TapiocaMeshValueView* data;
    size_t count;
} TapiocaMeshValueArray;
typedef struct {
    int32_t index;
    double transparency;
} TapiocaMaterialTransparency;
typedef struct {
    int32_t index, externalReference;
    TapiocaTextView name;
} TapiocaMaterialIdentity;
typedef struct {
    int32_t index;
    TapiocaTextView name, guid;
} TapiocaSurfaceAttribute;
typedef struct {
    const TapiocaMaterialTransparency* data;
    size_t count;
} TapiocaTransparencyArray;
typedef struct {
    const TapiocaMaterialIdentity* data;
    size_t count;
} TapiocaMaterialIdentityArray;
typedef struct {
    const TapiocaSurfaceAttribute* data;
    size_t count;
} TapiocaSurfaceAttributeArray;
typedef struct {
    uint64_t id, captureStamp;
    TapiocaTextView scope;
    uint32_t completeModel;
    TapiocaMeshValueArray meshes;
    TapiocaTransparencyArray materialTransparency;
    TapiocaMaterialIdentityArray materialIdentity;
    TapiocaSurfaceAttributeArray surfaceAttributes;
} TapiocaSnapshotValueView;
typedef struct {
    TapiocaSnapshotValueView snapshot;
    uint64_t expectedCaptureStamp; /* Zero: detached/synthetic; nonzero: require matching capture. */
    uint32_t requireCatalogs;      /* Require positive face pool/surface references; transparency stays optional. */
} TapiocaSnapshotValueRequest;

/* S2b affine vectors: row-major 3x4, never a projective 4x4 matrix.
 * Input/output counts are interleaved xyz scalar counts, not vector counts. */
typedef struct {
    double m[3][4];
} TapiocaAffineTransform;
enum { TAPIOCA_VECTOR_POINT = 1, TAPIOCA_VECTOR_DIRECTION = 2, TAPIOCA_VECTOR_NORMAL = 3 };
typedef struct {
    TapiocaAffineTransform transform;
    uint32_t mode;
    TapiocaDoubleView values;
} TapiocaAffineRequest;
typedef struct {
    double* data;
    size_t capacity;
    uint32_t* flipsOrientation;
} TapiocaAffineOutput;

enum {
    TAPIOCA_BUILD_TRANSLATION = 1,
    TAPIOCA_BUILD_ROTATION = 2,
    TAPIOCA_BUILD_SCALING = 3,
    TAPIOCA_BUILD_MIRRORING = 4
};
typedef struct {
    uint32_t kind;
    double origin[3]; /* Pivot in world metres; ignored for translation. */
    double vector[3]; /* Displacement metres / rotation axis / factors / mirror normal. */
    double radians;   /* Rotation only; ignored otherwise. */
} TapiocaTransformBuilderRequest;

/* Neutral legacy plane. Not an editor WorkingPlane/provenance/identity carrier. */
typedef struct {
    double origin[3], normal[3], u[3];
} TapiocaPlane;
typedef struct {
    TapiocaPlane plane; /* Exact retained input, not a normalized replacement. */
    double u[3], v[3];  /* Derived dimensionless in-plane axes. */
} TapiocaPlaneFrame;
typedef struct {
    TapiocaPlane plane;
    TapiocaDoubleView points;
} TapiocaPlaneRequest;
typedef struct {
    TapiocaPlaneFrame* frame; /* One required exclusive writable record. */
    double* points;
    size_t scalarCapacity; /* f64 scalars, not xyz points. */
} TapiocaPlaneOutput;

/* S2b polyline measurement/sampling; no implicit closure, welding or identity. */
typedef struct {
    TapiocaDoubleView points;     /* At least two xyz points, world metres. */
    TapiocaDoubleView parameters; /* Finite length fractions, clamped to [0,1]. */
} TapiocaPolylineRequest;
typedef struct {
    double* cumulative;
    size_t cumulativeCapacity; /* One distance per input vertex. */
    double* points;
    double* tangents;
    size_t sampleScalarCapacity; /* Scalar capacity of EACH xyz output buffer. */
    double* totalLength;         /* Required even for an empty parameter array. */
} TapiocaPolylineOutput;

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

/* Validate complete detached values without repair/geometry copies/retained borrows.
 * Snapshot/catalog validation is not authored provenance or proof of current
 * host state. C++ rechecks publication epochs; expectedCaptureStamp is separate
 * from the editor document revision and never inferred from input metadata. */
TapiocaStatus tapioca_validate_mesh_value (const TapiocaMeshValueView* mesh, char* error, size_t errorCapacity);
TapiocaStatus tapioca_validate_snapshot_value (const TapiocaSnapshotValueRequest* request, char* error,
                                               size_t errorCapacity);

/* Apply one affine transform to a batch of positions/directions/normals.
 * Positions translate; directions do not; normals use the inverse transpose
 * without renormalization. Exact singular normal transforms retain the input,
 * matching the legacy C++ contract. Non-finite input/derived values refuse.
 * flipsOrientation is required u32 storage (0/1); it informs a future mesh
 * caller's winding policy, but this primitive changes no topology or identity.
 * Both writable outputs must be live, aligned, disjoint from each other, both
 * descriptors, all inputs and error. Capacity may exceed input scalar count;
 * only that prefix is written. All outputs remain unchanged on refusal/panic.
 * Inputs are borrowed; Rust computes a temporary result and commits only after
 * complete validation. No Rust allocation escapes. OOM/invalid-memory caveats
 * apply. Bulk calls belong on workers; no latency guarantee or consumer flip. */
TapiocaStatus tapioca_transform_vectors (const TapiocaAffineRequest* request, const TapiocaAffineOutput* output,
                                         char* error, size_t errorCapacity);

/* Compose in application order: [a,b] applies a then b, folding from identity.
 * Empty input returns identity. Finite
 * coefficients/determinants are required
 * for each input and intermediate result; singular transforms are valid.
 *
 * count is transforms, not scalars. Nonempty input is immutable live storage;
 * output is one live aligned exclusively
 * writable transform, disjoint from
 * inputs and error. Null input is allowed only at count zero. Output remains
 *
 * unchanged on refusal/contained panic. Bulk chains belong on workers until
 * measured. No identity/topology,
 * allocation or borrowed view crosses ownership. */
TapiocaStatus tapioca_compose_transforms (const TapiocaAffineTransform* transforms, size_t count,
                                          TapiocaAffineTransform* output, char* error, size_t errorCapacity);

/* Allocation-free transform builder; explicit tagged fields above. Only consumed
 * fields must be finite.
 * Rotation/mirror normalize via the C++ squared norm;
 * zero/underflowed/non-finite lengths refuse. Scale factors must
 * be nonzero.
 * Finite coefficients, ordered pivot sums and determinant are required; a finite
 * determinant
 * underflowing to zero is allowed, as in the existing affine gate.
 * Output is one live aligned exclusive writable
 * record, unchanged on refusal or
 * contained panic, disjoint from initialized immutable request and error storage.
 *
 * Common foreign-memory/ownership/panic caveats apply. No geometry/identity
 * changes or production switch; bounded
 * constant work, worker until measured. */
TapiocaStatus tapioca_build_transform (const TapiocaTransformBuilderRequest* request, TapiocaAffineTransform* output,
                                       char* error, size_t errorCapacity);

/* Derive plane axes and project a flat xyz batch in world metres. Empty points
 * perform frame/value round-trip only.
 * Finite normals with length <1e-12 use
 * world Z; finite short/parallel authored u uses legacy normal-derived seed
 * axes.
 * Normalization multiplies by reciprocal length, preserving the editor oracle.
 * Raw origin/normal/u
 * round-trip unchanged. Non-finite inputs/arithmetic refuse.
 * Both outputs, including ENTIRE point capacity, must be
 * live aligned exclusively
 * writable, mutually disjoint and disjoint from ALL inputs/descriptors/error.
 * Null point
 * arrays require zero count/capacity; frame is always required. Only
 * the requested prefix and complete frame publish
 * on success; all outputs stay
 * unchanged on refusal/contained panic. Common foreign-memory/OOM/abort caveats
 *
 * apply. Worker by default until measured; no editor state or consumer switch. */
TapiocaStatus tapioca_project_to_plane (const TapiocaPlaneRequest* request, const TapiocaPlaneOutput* output,
                                        char* error, size_t errorCapacity);

/* Measure 3D lengths and sample by arc length, not by vertex index. No closure
 * is inferred. Exact joins select the
 * incoming segment; zero-length curves or
 * selected segments use +X tangents. Non-finite input/derived values
 * refuse.
 * Empty parameters measure only. Outputs are caller-owned, live, aligned and
 * mutually disjoint, and
 * disjoint from ALL inputs/descriptors/error storage.
 * Writes only vertex-count cumulative entries and
 * parameter-count xyz samples;
 * all outputs remain unchanged on refusal/contained panic. Null sample buffers
 *
 * require zero capacity. Ownership/panic/foreign-memory caveats above apply.
 * Bulk calls use workers; no D14
 * certification or production consumer switch. */
TapiocaStatus tapioca_sample_polyline (const TapiocaPolylineRequest* request, const TapiocaPolylineOutput* output,
                                       char* error, size_t errorCapacity);

/* Additive ABI-v1 arc POD. World metres, radians, dimensionless normal.
 * Signed sweep, including multiple turns;
 * adaptive C++ frame and segment order.
 * No local-origin conversion, endpoint snap, authored IDs or ArcPolyline
 * carrier. */
typedef struct {
    double centre[3], normal[3];
    double radius, startRadians, sweepRadians;
    int32_t segments; /* 2..4096 inclusive. */
} TapiocaArcRequest;

/* Writes exactly (segments+1)*3 interleaved xyz scalars. Capacity is scalar f64s,
 * not points. Request must be
 * initialized immutable live storage; output must be
 * aligned live exclusively writable storage, disjoint (ENTIRE
 * capacity) from
 * request and error. NULL output requires zero capacity, then refuses too-small.
 * Finite inputs,
 * positive radius, nonzero sweep and usable squared normal length
 * are required. Non-finite arithmetic refuses. All
 * output unchanged on refusal
 * or contained panic; no allocation/borrow escapes. Common foreign-memory and
 *
 * OOM/abort caveats apply. Worker by default until measured; no consumer flip. */
TapiocaStatus tapioca_make_arc (const TapiocaArcRequest* request, double* output, size_t scalarCapacity, char* error,
                                size_t errorCapacity);

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
