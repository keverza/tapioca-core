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

// S3a.2 terrain: u32 indices and f64 world metres. TIN provenance domains=3:
// input points as vertices, constraint segments flattened in constraint order
// as edges, zero faces/patches. Split provenance domains=7: mesh THEN line
// vertices, line segments as edges, baseline triangles as faces, zero patches.
// Provenance validation is mandatory. Caller retains the immutable input table;
// output supports reference it by input index, never newly allocated semantic ID.
// No spade type crosses C.
enum { TAPIOCA_TERRAIN_BOUNDARY = 1, TAPIOCA_TERRAIN_HOLE = 2, TAPIOCA_TERRAIN_BREAKLINE = 3 };
typedef struct {
    uint32_t role, offset, count;
} TapiocaTerrainConstraint;
typedef struct {
    double minAngleDegrees, maxAreaSquareMetres;
    uint32_t maxAddedVertices, enabled, hasMaxArea;
} TapiocaTerrainRefinement;
typedef struct {
    TapiocaDoubleView points;
    struct {
        const TapiocaTerrainConstraint* data;
        size_t count;
    } constraints;
    TapiocaIndexView constraintVertices;
    double zTolerance;
    TapiocaTerrainRefinement refinement;
    TapiocaProvenanceRequest provenance;
} TapiocaTerrainRequest;
typedef struct {
    TapiocaDoubleView points;
    TapiocaIndexView triangles;
    TapiocaDoubleView line, arcAngles;
    TapiocaProvenanceRequest provenance;
} TapiocaTerrainSplitRequest;
typedef struct {
    size_t points, triangles, mappings;
} TapiocaTerrainCounts;

// Support rows preserve kernel ordering; unused values are zero. Fields:
// 1 Input point: index=input vertex; v0=output vertex.
// 2 TIN face: index=output face; v0=boundary constraint (UINT32_MAX for hull).
//   Contributors resolve through corner support and bounding ring rows.
// 3 Crossing: index=output vertex; v0/v1=constraint/segment A, v2/v3=B;
//   v4/v5=flattened input edge indices A/B.
// 4 Constraint edge: index=constraint; v0=segment, v1/v2=output endpoints,
//   v3=kept (0/1), v4=flattened input edge index.
// 5 Steiner segment: index=output vertex; v0/v1=constraint/segment,
//   v2=flattened input edge index. One row per support, including overlaps.
// 6 Steiner edge: index=output vertex; v0/v1=original output endpoints.
// 7 Steiner face: index=output vertex; v0/v1/v2=original output face corners.
// 8 Split face: index=output face; v0=input parent face, v1=inside (0/1).
// 9 Split vertex: index=output vertex; v0=origin (1 line vertex, 2 crossing),
//   v1=line vertex/segment, v2=host (1 mesh edge, 2 mesh face), v3/v4=host
//   endpoints or v3=host face with v4=0. Unchanged mesh vertices map directly.
// 10 Line vertex: index=input line vertex; v0=output vertex.
// 11 Line edge: index=input line segment; v0/v1=output endpoints.
// Original output vertices/corners in 6/7 resolve recursively via 1/3.
// Line vertex i resolves input provenance vertex meshVertexCount+i.
// These rows allocate no authored identity, persistent patch key or native GUID.
typedef struct {
    uint32_t kind, index, values[6];
} TapiocaTerrainMapping;
typedef struct {
    TapiocaTerrainCounts* counts;
    double* points;
    size_t pointCapacity;
    uint32_t* triangles;
    size_t triangleCapacity;
    TapiocaTerrainMapping* mappings;
    size_t mappingCapacity;
    uint32_t queryOnly;
} TapiocaTerrainOutput;

typedef struct {
    TapiocaDoubleView points;
    TapiocaProvenanceRequest provenance;
} TapiocaAttributedPolygonRequest;

// S3b.3 grading solve: weighted sparse least squares over a term pool; fixed
// unknowns are eliminated. constrained=0 requires empty bounds/differences/start.
// Infinite bound/difference sides are open. Values: one per unknown.
typedef struct {
    double coefficient;
    uint32_t unknown, reserved;
} TapiocaGradingTerm;
typedef struct {
    double target, weight;
    uint32_t first, count;
} TapiocaGradingRow;
typedef struct {
    double value;
    uint32_t unknown, reserved;
} TapiocaGradingFixed;
typedef struct {
    double lower, upper;
    uint32_t unknown, reserved;
} TapiocaGradingBound;
typedef struct {
    double min, max; /* min <= x[a] - x[b] <= max */
    uint32_t a, b;
} TapiocaGradingDifference;
typedef struct {
    struct {
        const TapiocaGradingTerm* data;
        size_t count;
    } terms;
    struct {
        const TapiocaGradingRow* data;
        size_t count;
    } rows;
    struct {
        const TapiocaGradingFixed* data;
        size_t count;
    } fixed;
    struct {
        const TapiocaGradingBound* data;
        size_t count;
    } bounds;
    struct {
        const TapiocaGradingDifference* data;
        size_t count;
    } differences;
    TapiocaDoubleView start; /* Empty or one per unknown (constrained only). */
    double feasibility, residual;
    uint32_t unknowns, maxIterations, constrained, reserved;
} TapiocaGradingRequest;
typedef struct {
    double objective;
    uint32_t iterations, factorizations, active, reserved;
} TapiocaGradingSummary;
typedef struct {
    TapiocaGradingSummary* summary; /* Required. */
    double* values;
    size_t valueCapacity; /* At least unknowns. */
} TapiocaGradingOutput;

// S3b.3 terrain field, edits and volumes over upward faces (unit normal z >
// 0.001). Sample flags: bit0 on surface (else all zero), bit1 aspect valid, bits
// 2-3 / 4-5 x / y gradient method (1 central, 2 one-sided, 3 face plane).
enum { TAPIOCA_SAMPLE_ON_SURFACE = 1, TAPIOCA_SAMPLE_HAS_ASPECT = 2 };
typedef struct {
    TapiocaDoubleView points;
    TapiocaIndexView triangles;
    TapiocaDoubleView queries; /* x y pairs */
    double step;               /* Gradient step, metres. */
} TapiocaTerrainSampleRequest;
typedef struct {
    double height, normal[3], gradient[2], slope, aspect; /* Radians; aspect from +y clockwise. */
    uint32_t face, flags;
} TapiocaTerrainSample;
typedef struct {
    double min[3], max[3], area;
    uint32_t faces, excluded;
} TapiocaTerrainFieldSummary;
typedef struct {
    TapiocaTerrainFieldSummary* summary; /* Required. */
    TapiocaTerrainSample* samples;
    size_t sampleCapacity; /* At least queries.count / 2. */
} TapiocaTerrainSampleOutput;

// Edits move vertex heights only; indices and triangles keep their identity.
// Provenance domains=5 (mesh vertices, mesh triangles) is mandatory. Slope kind:
// 1 angle (a radians), 2 percent (a), 3 rise:run (a:b), 4 height (a m over b m).
// Edit target: 1 point, 2 curve, 3 area, 4 path, 5 surface. mode: 1 absolute,
// 2 relative. effect: 1 cut, 2 fill, 3 both. transition: 1 falloff, 2 daylight
// (absolute only). levelKind: 1 height, 2 flatten. crossfallHand: 0 none, 1 left,
// 2 right. Curves, paths and rings are xyz scalar ranges of the geometry pool.
// owners[v] is UINT32_MAX for an unmoved vertex; face rows are at most three
// per triangle and faceCount is published.
typedef struct {
    double a, b;
    uint32_t kind, reserved;
} TapiocaTerrainSlope;
typedef struct {
    TapiocaTerrainSlope slope;
    double extension;
    uint32_t hasExtension, reserved;
} TapiocaTerrainDaylight;
typedef struct {
    double position[3], width, level, blur, maxDistance, strength;
    TapiocaTerrainDaylight cut, fill;
    TapiocaTerrainSlope crossfall;
    uint32_t target, mode, effect, transition, geometryOffset, geometryCount, levelKind, crossfallHand, surface,
        reserved;
} TapiocaTerrainEdit;
typedef struct {
    TapiocaDoubleView points;
    TapiocaIndexView triangles;
} TapiocaTerrainSurfaceView;
typedef struct {
    uint32_t offset, count;
} TapiocaTerrainRing;
typedef struct {
    TapiocaDoubleView points;
    TapiocaIndexView triangles;
    struct {
        const TapiocaTerrainEdit* data;
        size_t count;
    } edits;
    TapiocaDoubleView geometry;
    struct {
        const TapiocaTerrainSurfaceView* data;
        size_t count;
    } surfaces;
    struct {
        const TapiocaTerrainRing* data;
        size_t count;
    } protectedRings;
    TapiocaTerrainRing clip;
    uint32_t hasClip, hasSmoothing;
    double smoothing;
    TapiocaProvenanceRequest provenance;
} TapiocaTerrainEditRequest;
typedef struct {
    uint32_t face, edit;
} TapiocaTerrainFaceOwner;
typedef struct {
    uint32_t moved, extensionClipped, limitClipped, reserved;
} TapiocaTerrainEditReport;
typedef struct {
    size_t* faceCount; /* Required. */
    double* heights;
    size_t heightCapacity;
    uint32_t* owners;
    size_t ownerCapacity;
    TapiocaTerrainFaceOwner* faces;
    size_t faceCapacity;
    TapiocaTerrainEditReport* reports;
    size_t reportCapacity;
} TapiocaTerrainEditOutput;

// Volumes: oracle grid semantics (cell count rule, partial cells, centres or
// k x k sub-centres), diff = proposed - existing. proposed: 1 mesh, 2 plane,
// 3 existing mesh with heights + owners (per-edit rows, ascending edit).
// Outside or on the property ring is context.
typedef struct {
    TapiocaDoubleView points;
    TapiocaIndexView triangles;
    TapiocaDoubleView proposedPoints;
    TapiocaIndexView proposedTriangles;
    TapiocaDoubleView heights;
    TapiocaIndexView owners;
    TapiocaDoubleView property;
    double domain[4]; /* xMin yMin xMax yMax when hasDomain. */
    double plane, cell;
    uint32_t proposed, subdivisions, hasDomain, reserved;
} TapiocaTerrainVolumeRequest;
typedef struct {
    double cut, fill, net, area;
    uint32_t samples, excluded;
} TapiocaTerrainVolumes;
typedef struct {
    double cut, fill, net, area;
    uint32_t edit, reserved;
} TapiocaTerrainEditVolumes;
typedef struct {
    TapiocaTerrainVolumes* site;    /* Required. */
    TapiocaTerrainVolumes* context; /* Required. */
    uint32_t* cells;                /* Required: two counts. */
    size_t* editCount;              /* Required. */
    TapiocaTerrainEditVolumes* edits;
    size_t editCapacity;
} TapiocaTerrainVolumeOutput;

// S4.2 hybrid band: one path band on a base mesh inside a closed straight region
// ring, rebuilt as a constrained TIN (band edges, shoulder ends, daylight lines and
// end sections as breaklines; every base edge kept), with exact per-triangle
// cut/fill (site inside the region, context outside). Provenance domains=7 as for
// split: mesh THEN ring vertices, ring segments as edges, base triangles as faces.
// Stations carry no entity rows. effect: 1 cut, 2 fill, 3 both. crossfallHand: 0
// level, 1 left, 2 right, 3 crown (both halves fall from the centreline). Line kind:
// 1 band edge, 2 shoulder end, 3 daylight, 4 end section, 5 crowned centreline (a
// crease; sections then carry a centre vertex); 7 internal sampled cross-section
// (root = input station index, excluding first/last). Sections span band, shoulders
// and daylight, so sparse source triangles cannot skip an authored station.
// side 1 left, 2 right (0 for an
// end or centreline); root = daylight root outward, or 0 first / 1 last end. sides: [left, right] per station, roots 0
// = at grade. owners[v] is 0 when the band changed the height, else UINT32_MAX; base[v] is the base height. Support
// rows reuse TapiocaTerrainMapping: 9 Split vertex (as above; line = region ring). 12 Region face: index=output face;
// v0=input parent face, v1=inside, v2=kept
//    (verbatim input triangle).
// 13 Region line vertex: index=output vertex; v0=line, v1=line vertex.
// 14 Region crossing: index=output vertex; v0=1 base edge (v1/v2 output
//    endpoints, ascending) or 2 line (v1/v2 line/segment). Two rows per vertex.
// 15 Line vertex: index=line; v0=station ordinal, v1=output vertex.
// 16 Line edge: index=line; v0=segment, v1/v2=output endpoints in line order.
// 17 Ring vertex: index=ring position; v0=output vertex (closed chain).
// Output vertices below the input count are the input vertices unchanged.
typedef struct {
    double position[3], halfLeft, halfRight, shoulderLeft, shoulderRight, cutLimit, fillLimit;
    TapiocaTerrainSlope cut, fill, crossfall;
    uint32_t crossfallHand, reserved;
} TapiocaTerrainStation;
typedef struct {
    TapiocaDoubleView points;
    TapiocaIndexView triangles;
    TapiocaDoubleView region;
    struct {
        const TapiocaTerrainStation* data;
        size_t count;
    } stations;
    double tolerance;
    uint32_t effect, reserved;
    TapiocaProvenanceRequest provenance;
} TapiocaTerrainHybridRequest;
typedef struct {
    size_t points, triangles, mappings, lines;
} TapiocaTerrainHybridCounts;
typedef struct {
    double cut, fill, net, area;
    uint32_t triangles, reserved;
} TapiocaTerrainHybridVolumes;
typedef struct {
    uint32_t kind, side, root, reserved;
} TapiocaTerrainHybridLine;
typedef struct {
    double first, last;
    uint32_t roots, reserved;
} TapiocaTerrainHybridSide;
typedef struct {
    TapiocaTerrainHybridCounts* counts;   /* Required. */
    TapiocaTerrainHybridVolumes* site;    /* Required. */
    TapiocaTerrainHybridVolumes* context; /* Required. */
    double* points;
    size_t pointCapacity;
    double* base;
    size_t baseCapacity; /* At least points / 3. */
    uint32_t* owners;
    size_t ownerCapacity; /* At least points / 3. */
    uint32_t* triangles;
    size_t triangleCapacity;
    TapiocaTerrainMapping* mappings;
    size_t mappingCapacity;
    TapiocaTerrainHybridLine* lines;
    size_t lineCapacity;
    TapiocaTerrainHybridSide* sides;
    size_t sideCapacity; /* At least 2 x stations. */
    uint32_t queryOnly, reserved;
} TapiocaTerrainHybridOutput;

// Fixed-control build, additive to the pinned hybrid records. One mandatory
// domains=7 catalog: mesh, ring, all line vertices, then independent points;
// edges: ring segments then line segments in line order; faces: base triangles.
// Line ranges partition linePoints in order, in xyz scalar units, >=6 each.
// All controls must be strictly inside the region. Existing query/fill,
// full-capacity alias preflight and atomic refusal contracts apply.
typedef struct {
    TapiocaTerrainHybridRequest band;
    TapiocaDoubleView linePoints;
    struct {
        const TapiocaTerrainRing* data;
        size_t count;
    } lines;
    TapiocaDoubleView points;
} TapiocaTerrainHybridLockedRequest;
// Output adds line kind 6 (side=0, root=input control line) and mapping kinds:
// 20 Region point: index=output vertex, v0=independent control point.
// 21 Point input remap: index=control point, v0=output vertex (all contributors).
// 22 Fixed vertex support: index=output vertex, v0=1 line segment / 2 point,
//    v1=input line/point, v2=segment (0 for points). One row per contributor.
// Unused values zero. Fixed vertices have no band owner; their height in points
// is exact. Compatible controls retain the first input height (lines then points).
// Daylight height bits remain exact, even within the compatibility tolerance.
typedef struct {
    double position[3], magnitude;
    uint32_t code, station, side, hard, vertex;
    uint32_t controlKind, controlIndex, controlSegment;
    uint32_t otherKind, otherIndex, otherSegment, reserved;
} TapiocaTerrainHybridDiagnostic;
typedef struct {
    size_t* count;
    TapiocaTerrainHybridDiagnostic* rows;
    size_t capacity;
    uint32_t queryOnly, reserved;
} TapiocaTerrainHybridDiagnosticOutput;
// Assessment carries station-side codes 1..5 and fixed conflicts (6). Hard band
// rows (1..4) prevent the candidate TIN: fixed assessment is then incomplete.
// Otherwise: each disagreeing contributor versus the canonical first height,
// then each canonical fixed vertex versus grading. Deterministic, no duplicate
// segment contributor from adjacent split edges; not all unordered control pairs.
// For code 6: station=UINT32_MAX, side=0, hard=1, vertex=candidate vertex;
// control kind 1 segment / 2 point; other kind 0 means grading disagrees.
// magnitude=other/proposed minus fixed height. Other fields unused/zero.
// Assessment builds a TIN; worker-classified, not a cheap per-point query.

// S4.3 located conflicts, additive to the build/edit records and counters.
// Band conflict code: 1 DaylightNotReached (magnitude: z_cone - z_base where the
// search stopped), 2 DaylightBeyondLimit (reach needed, metres from the edge),
// 3 VerticalFace (0), 4 DaylightTopology (root count here, 0 at grade), 5
// EdgeContradictsEffect (z_edge - z_base; soft, the band is held at the ground).
// side 1 left, 2 right; hard 1 when the build refuses on it. position: the band
// edge, or where a face search stopped. A request the build refuses still
// assesses successfully. Edit clip kind: 1 beyond a daylight extension, 2 beyond
// the maximum distance; rows by vertex then edit, summing to the edit counters.
typedef struct {
    double position[3], magnitude;
    uint32_t code, station, side, hard;
} TapiocaTerrainConflict;
typedef struct {
    double distance, withheld; /* From the edit's core; signed metres after strength. */
    uint32_t edit, vertex, kind, reserved;
} TapiocaTerrainClip;
typedef struct {
    size_t* count; /* Required. */
    TapiocaTerrainConflict* rows;
    size_t capacity;
    uint32_t queryOnly, reserved;
} TapiocaTerrainConflictOutput;
typedef struct {
    size_t* count; /* Required. */
    TapiocaTerrainClip* rows;
    size_t capacity;
    uint32_t queryOnly, reserved;
} TapiocaTerrainClipOutput;

// Native solid -> compact upward plan surface. Input provenance domains=5.
// Uses TapiocaTerrainOutput query/fill. Mapping 18: index=output vertex,
// values[0]=input vertex (one per contributor, including exact-position merges).
// Mapping 19: index=output face, values[0]=input face. All other values zero.
// First referenced input vertex order/bits win; faces keep native order.
// Same field upward rule; unequal heights at one xy and bad topology refuse.
// Native height-field input required; arbitrary overlapping sheets unverified.
typedef struct {
    TapiocaDoubleView points;
    TapiocaIndexView triangles;
    TapiocaProvenanceRequest provenance;
} TapiocaTerrainSurfaceRequest;

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

typedef struct {
    TapiocaDoubleView points;
    int32_t count;        /* Segment count, 1..100000. */
    uint32_t includeEnds; /* Exactly 0 or 1. */
} TapiocaDividePolylineRequest;

/* Equal arc-length division: N+1 xyz positions with ends, N-1 without. No
 * inferred closure, repair or identity
 * generation. Finite >=2 xyz input points;
 * count=1 without ends returns empty without evaluating unused lengths.
 *
 * Otherwise non-finite used arithmetic refuses. All errors return ARG, contained
 * unwind INTERNAL. All output stays
 * unchanged on failure; only the requested
 * prefix publishes on success. ENTIRE output capacity/error must be live
 * aligned
 * exclusive writable and mutually disjoint, and disjoint from descriptor/points.
 * Input storage is live
 * initialized immutable and call-scoped. Null arrays only
 * at zero count/capacity; zero output capacity is valid only
 * for an empty result.
 * Shared foreign-memory/OOM/abort caveats apply. Worker until measured. */
TapiocaStatus tapioca_divide_polyline (const TapiocaDividePolylineRequest* request, double* output,
                                       size_t scalarCapacity, char* error, size_t errorCapacity);

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

/* S2b exact single-contour value copy, not polygon modelling/topology validation.
 * Empty or >=3 finite xyz vertices
 * in world metres, no repair/welding/closure
 * removal. Nonempty: N stored vertices, N implicit last-to-first edges,
 * one face;
 * empty: all counts zero. Provenance domains exactly 7, patches zero; complete
 * attribution required.
 * Existing structural validator and requireObject policy
 * apply. No identity is invented or changed; caller RETAINS
 * the exact input table
 * for the unchanged entity ordering. No hole-role inference/geometric ancestry
 *
 * certification. Degenerate, nonplanar and self-intersecting values can copy.
 * Descriptor and every point/provenance
 * pool must be live initialized immutable
 * allocations; entire output scalar capacity and error storage must be live

 * * aligned exclusively writable, mutually disjoint and disjoint from ALL inputs.
 * Null arrays require zero
 * count/capacity. Only requested prefix publishes on
 * success; all output unchanged on refusal/panic. ERR_ARG for
 * geometry/transport,
 * ERR_STATE for attribution, shared panic/OOM/abort/foreign-memory caveats apply.
 * O(points +
 * provenance validation), worker by default until measured. */
TapiocaStatus tapioca_copy_attributed_polygon (const TapiocaAttributedPolygonRequest* request, double* output,
                                               size_t scalarCapacity, char* error, size_t errorCapacity);

/* G1 additive mesh preparation. No production family flip, semantic ID creation
 * or SDK capture workflow. Coordinates
 * are world metres f64, RH Z-up. Normals
 * are dimensionless. Bounds include unused supplied vertices, NOT un-emitted

 * * source-body vertices; caller retains capture bounds/identity/sidecars.
 * Both calls are worker-classified until
 * measured. */
typedef struct {
    int32_t sourceVertex;          /* Opaque signed body-local key, not a position offset. */
    double position[3], normal[3]; /* GIVEN normal, no normalization here. */
} TapiocaWeldCorner;
typedef struct {
    const TapiocaWeldCorner* data;
    size_t count;
} TapiocaWeldCornerView;
typedef struct {
    const uint64_t* data;
    size_t count;
} TapiocaUint64View;
typedef struct {
    TapiocaWeldCornerView corners;
    TapiocaUint64View bodyOffsets; /* [0,...,corners.count], nondecreasing; empty bodies valid. */
} TapiocaWeldRequest;
typedef struct {
    size_t* vertexCount; /* Required; actual output vertex count, not scalars. */
    double* positions;
    float* normals;
    size_t scalarCapacity; /* EACH xyz buffer: at least 3*corners.count. */
    uint32_t* cornerVertices;
    size_t cornerCapacity; /* At least corners.count, even when corners deduplicate. */
} TapiocaWeldOutput;

typedef struct {
    uint64_t offset, count;
} TapiocaEdgeRange;
typedef struct {
    const TapiocaEdgeRange* data;
    size_t count;
} TapiocaEdgeRangeView;
typedef struct {
    TapiocaDoubleView positions;
    TapiocaFloatView normals; /* Empty or per-vertex xyz, shape/finite checked. */
    TapiocaIndexView triangles;
    TapiocaUint64View visibleEdges;  /* (min << 32)|max, SAME index domain as triangles. */
    TapiocaEdgeRangeView edgeRanges; /* Empty: global pool; else one locally sorted range/triangle. */
} TapiocaMeshAnalysisRequest;
typedef struct {
    TapiocaAabb bounds;
    uint32_t normalsUsable; /* 0 or 1: existing C++ 0.9..1.1 length test. */
} TapiocaMeshAnalysisSummary;
typedef struct {
    TapiocaMeshAnalysisSummary* summary; /* Required even for empty input. */
    float* smoothNormals;
    size_t scalarCapacity;   /* At least positions.count. */
    uint8_t* roundoffZero;   /* 0/1 flag per triangle; no topology change. */
    uint8_t* wireMasks;      /* bit0 v1-v2, bit1 v2-v0, bit2 v0-v1. */
    size_t triangleCapacity; /* Capacity of EACH byte buffer. */
} TapiocaMeshAnalysisOutput;

/* First-encounter (sourceVertex,round(normal*1e4)) weld; halfway AWAY from zero.
 * Reset key table at each body
 * offset. No position proximity/averaging, source
 * rewrite or semantic-ID allocation. Complete cornerVertices remap
 * retains ALL
 * contributors. Count <= UINT32_MAX; finite input and representable i32 keys.
 * Analysis preserves
 * order/topology; smooth f32 fallback deliberately smooths
 * hard edges. Roundoff threshold is 8*DBL_EPSILON after
 * per-triangle edge scaling,
 * not an area/contact tolerance. Canonical visibility keys/ranges must be sorted
 * per
 * selected range; duplicates allowed. Non-finite used arithmetic refuses.
 *
 * All inputs/descriptors live aligned
 * initialized immutable for the call. Full
 * declared output capacities and required count/summary are live
 * exclusively
 * writable, mutually disjoint, and disjoint from ALL inputs/descriptors/error.
 * Null arrays require
 * zero count/capacity. Only result prefixes publish; tails
 * untouched. Every output stays unchanged on
 * refusal/contained panic. ERR_ARG
 * for shape/geometry/arithmetic, ERR_INTERNAL for caught unwind. No borrow or
 *
 * allocation escapes. Shared foreign-allocation/OOM/abort limitations apply. */
TapiocaStatus tapioca_weld_corners (const TapiocaWeldRequest* request, const TapiocaWeldOutput* output, char* error,
                                    size_t errorCapacity);
TapiocaStatus tapioca_analyze_mesh (const TapiocaMeshAnalysisRequest* request, const TapiocaMeshAnalysisOutput* output,
                                    char* error, size_t errorCapacity);

// QueryOnly=1 computes and publishes only counts; all capacities must be zero.
// QueryOnly=0 recomputes with unchanged input and requires full capacities.
// Counts and array prefixes publish atomically, never on refusal/unwind.
// Full capacities, counts, input descriptors/pools and error must be disjoint
// wherever either is writable. Null arrays need zero count/capacity. Caller
// guarantees live aligned allocations and initialized immutable input. No pointer,
// Rust allocation or handle is retained. Unwind is contained, not OOM/abort/UB.
// Worker-classified: query/fill compute and allocate twice. Attribution refusals
// return STATE with schema text; geometry/parameter refusals return ARG.
TapiocaStatus tapioca_terrain_triangulate (const TapiocaTerrainRequest* request, const TapiocaTerrainOutput* output,
                                           char* error, size_t errorCapacity);
TapiocaStatus tapioca_terrain_split_mesh (const TapiocaTerrainSplitRequest* request, const TapiocaTerrainOutput* output,
                                          char* error, size_t errorCapacity);
TapiocaStatus tapioca_terrain_surface (const TapiocaTerrainSurfaceRequest* request, const TapiocaTerrainOutput* output,
                                       char* error, size_t errorCapacity);

// S3b.3: single compute, caller-sized outputs (no query mode). The same
// live/aligned/disjoint/atomic contract as above applies, including every
// nested surface pool of an edit. Outputs are unchanged on any refusal.
// Edit attribution refusals return STATE; all other refusals ARG.
// Worker-classified; each call allocates.
TapiocaStatus tapioca_grading_solve (const TapiocaGradingRequest* request, const TapiocaGradingOutput* output,
                                     char* error, size_t errorCapacity);
TapiocaStatus tapioca_terrain_sample (const TapiocaTerrainSampleRequest* request,
                                      const TapiocaTerrainSampleOutput* output, char* error, size_t errorCapacity);
TapiocaStatus tapioca_terrain_edit (const TapiocaTerrainEditRequest* request, const TapiocaTerrainEditOutput* output,
                                    char* error, size_t errorCapacity);
TapiocaStatus tapioca_terrain_volume (const TapiocaTerrainVolumeRequest* request,
                                      const TapiocaTerrainVolumeOutput* output, char* error, size_t errorCapacity);

// S4.2: query/fill like tapioca_terrain_triangulate (queryOnly=1 publishes only
// counts; all capacities zero). Atomic, unchanged on refusal. Attribution
// refusals return STATE; geometry/parameter refusals ARG with the kernel code
// (DaylightNotReached, HybridVertical, HybridDaylightTopology, Region*, Design*).
// Worker-classified; P1 rebuilds the whole region interior per call.
TapiocaStatus tapioca_terrain_hybrid_build (const TapiocaTerrainHybridRequest* request,
                                            const TapiocaTerrainHybridOutput* output, char* error,
                                            size_t errorCapacity);
TapiocaStatus tapioca_terrain_hybrid_locked_build (const TapiocaTerrainHybridLockedRequest* request,
                                                   const TapiocaTerrainHybridOutput* output, char* error,
                                                   size_t errorCapacity);
TapiocaStatus tapioca_terrain_hybrid_locked_assess (const TapiocaTerrainHybridLockedRequest* request,
                                                    const TapiocaTerrainHybridDiagnosticOutput* output, char* error,
                                                    size_t errorCapacity);
// S4.3: query/fill row exports with the same request records and contracts.
TapiocaStatus tapioca_terrain_hybrid_conflicts (const TapiocaTerrainHybridRequest* request,
                                                const TapiocaTerrainConflictOutput* output, char* error,
                                                size_t errorCapacity);
TapiocaStatus tapioca_terrain_edit_clips (const TapiocaTerrainEditRequest* request,
                                          const TapiocaTerrainClipOutput* output, char* error, size_t errorCapacity);

#ifdef __cplusplus
}
#endif

#endif
