#ifndef TAPIOCA_SUNSTUDY_RUST_FFI_H
#define TAPIOCA_SUNSTUDY_RUST_FFI_H

/* Sun-study C ABI v2, exported by the aggregate tapioca_core_ffi archive.
 * Only implemented exports are declared here. Shares TapiocaStatus and the
 * error-buffer envelope with GeometryRustFfi.h: every call returns a status,
 * writes a NUL-terminated UTF-8 diagnostic prefix on failure, leaves outputs
 * unchanged on refusal, and contains Rust panics (TAPIOCA_ERR_INTERNAL).
 * No SDK types, containers, native bool, exceptions or retained borrows.
 *
 * Every array is a call-scoped borrow. Writable regions (output arrays at
 * their full declared capacity, and output records) must be disjoint from
 * every other region and from the error buffer; inputs may alias each other.
 * Record layouts mirror the Rust repr(C) declarations field for field and are
 * size-checked on both sides. */

#include "Geometry/GeometryRustFfi.h"

#define TAPIOCA_SUNSTUDY_ABI_VERSION UINT32_C (2)

#ifdef __cplusplus
extern "C" {
#endif

/* One host-sampled timestep. direction points TO the sun, model space. */
typedef struct {
    int32_t hour;
    int32_t minute;
    double direction[3];
    double altitudeDegrees;
} TapiocaSunStep;

typedef struct {
    const TapiocaSunStep* data;
    size_t count;
} TapiocaSunStepView;

typedef struct {
    TapiocaSunStepView steps;
    int32_t timestepMinutes;   /* < 1 becomes 1 */
    double minAltitudeDegrees; /* steps at or below are dropped */
} TapiocaSunSeriesRequest;

typedef struct {
    size_t stepCount;
    size_t sourceStepCount;
    uint64_t version; /* FNV-1a content hash; 0 only when empty */
    int32_t timestepMinutes;
    double hoursPerStep;
    double daylightHours;
} TapiocaSunSeriesSummary;

typedef struct {
    TapiocaSunStep* steps; /* capacity >= request step count */
    size_t capacity;
    TapiocaSunSeriesSummary* summary;
} TapiocaSunSeriesOutput;

/* Horizon filter, normalisation and content version (SunSeries::FromSteps). */
TapiocaStatus tapioca_sunstudy_series (const TapiocaSunSeriesRequest* request, const TapiocaSunSeriesOutput* output,
                                       char* error, size_t errorCapacity);

typedef struct {
    double aabbMin[3];
    double aabbMax[3];
    double sunDir[3]; /* TO the sun; need not be unit length */
    double margin;
    uint32_t resolution;
    double minBias;
} TapiocaShadowCameraRequest;

/* valid == 0 (zero sun or inverted box): every other field is zero. */
typedef struct {
    double planes[6]; /* left, right, bottom, top, near, far; near may be < 0 */
    double extent[3]; /* world width, height, depth */
    double rightAxis[3];
    double upAxis[3];
    double lightAxis[3];
    double projection[16];          /* row-major */
    double lightViewProjection[16]; /* row-major, unshifted positions */
    double rtcOrigin[3];
    double rtcLightViewProjection[16]; /* positions with rtcOrigin subtracted */
    double texelWorldSize;
    double bias;
    double depthBiasNdc;
    uint8_t valid;
} TapiocaShadowCamera;

TapiocaStatus tapioca_sunstudy_shadow_camera (const TapiocaShadowCameraRequest* request, TapiocaShadowCamera* output,
                                              char* error, size_t errorCapacity);

/* valid == 0: refused (too many samples or maxWidth 0), width = height = 1. */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t valid;
} TapiocaAccumLayout;

TapiocaStatus tapioca_sunstudy_accum_layout (size_t sampleCount, uint32_t maxWidth, TapiocaAccumLayout* output,
                                             char* error, size_t errorCapacity);

typedef struct {
    double areaM2;
    double targetGrid;
    double coarseThreshold;
    double midThreshold;
} TapiocaGridLadderRequest;

typedef struct {
    double grids[3]; /* coarsest first; grids[count..] are 0 */
    uint32_t count;
} TapiocaGridLadder;

TapiocaStatus tapioca_sunstudy_grid_ladder (const TapiocaGridLadderRequest* request, TapiocaGridLadder* output,
                                            char* error, size_t errorCapacity);

typedef struct {
    double aabbMin[3];
    double aabbMax[3];
    double spacing;
    double pad; /* < 0 selects the automatic pad */
    double zOffset;
    size_t maxSamples;
} TapiocaGroundGridRequest;

/* valid == 0 (refused): every other field is zero. */
typedef struct {
    double pad;
    double groundZ;
    size_t columns;
    size_t rows;
    size_t sampleCount;
    uint8_t valid;
} TapiocaGroundGridSummary;

typedef struct {
    double* positions; /* NULL with capacity 0 asks for the shape only */
    size_t scalarCapacity;
    TapiocaGroundGridSummary* summary;
} TapiocaGroundGridOutput;

/* Regular, centred ground grid; normals are all +Z and are not transported. */
TapiocaStatus tapioca_sunstudy_ground_grid (const TapiocaGroundGridRequest* request,
                                            const TapiocaGroundGridOutput* output, char* error, size_t errorCapacity);

typedef struct {
    TapiocaDoubleView vertices; /* xyz f64 world metres */
    TapiocaIndexView triangles; /* triples; a trailing partial triple is ignored */
    TapiocaIndexView groups;    /* one id per face, or empty for one element */
    double weldTolerance;       /* <= 0 or NaN selects 1e-4 */
} TapiocaWindingRequest;

typedef struct {
    size_t groups;
    size_t closed;
    size_t flipped;
    size_t flippedTriangles;
    size_t indexCount;
} TapiocaWindingReport;

typedef struct {
    uint32_t* triangles; /* capacity >= complete input triples x 3 */
    size_t capacity;
    TapiocaWindingReport* report;
} TapiocaWindingOutput;

/* Flip every closed, inward-wound element (SunStudyWinding OrientOutward). */
TapiocaStatus tapioca_sunstudy_orient_outward (const TapiocaWindingRequest* request, const TapiocaWindingOutput* output,
                                               char* error, size_t errorCapacity);

typedef struct {
    uint64_t gpuMemoryBytes;
    uint64_t availableRamBytes;
    size_t steps;
    double occupancy;
    double analysedArea;
    uint32_t maxTextureDimension;
    uint8_t gpuKnown; /* 0 or 1 */
} TapiocaLimitsRequest;

typedef struct {
    size_t maxSamples;
    size_t caps[4]; /* sampler, system memory, GPU memory, texture; 0 unknown */
    uint64_t maxRays;
    double systemBytesPerSample;
    double gpuBytesPerTexel;
    double finestGridSpacing;
    uint32_t maxAtlasDimension;
    uint8_t binding; /* LimitBinding value */
} TapiocaLimits;

TapiocaStatus tapioca_sunstudy_limits (const TapiocaLimitsRequest* request, TapiocaLimits* output, char* error,
                                       size_t errorCapacity);

/* GUID lists: length-delimited UTF-8 texts; non-UTF-8 text is refused. */
typedef struct {
    const TapiocaTextView* data;
    size_t count;
} TapiocaTextArrayView;

typedef struct {
    TapiocaTextArrayView elements; /* one GUID per element, element order */
    TapiocaTextArrayView analysis;
    TapiocaTextArrayView context;
    TapiocaTextArrayView ignored;
} TapiocaRolesRequest;

typedef struct {
    size_t counts[3];    /* analysis, context, ignored elements */
    size_t unmatched[3]; /* distinct canonical picks matching no element */
    uint8_t analysisNamedButAbsent;
} TapiocaRolesSummary;

typedef struct {
    uint8_t* roles; /* one ElementRole byte per element; capacity >= elements */
    size_t capacity;
    TapiocaRolesSummary* summary;
} TapiocaRolesOutput;

/* ResolveElementRoles over GUID lists. */
TapiocaStatus tapioca_sunstudy_resolve_roles (const TapiocaRolesRequest* request, const TapiocaRolesOutput* output,
                                              char* error, size_t errorCapacity);

typedef struct {
    size_t triangleCount;
    TapiocaIntView triMaterial; /* may be shorter than triangleCount */
} TapiocaReceiverMeshView;

typedef struct {
    int32_t material;
    double transparency; /* model pool 0..1 */
} TapiocaSunStudyTransparency;

typedef struct {
    const TapiocaReceiverMeshView* meshes;
    size_t meshCount;
    TapiocaByteView roles; /* one ElementRole byte per mesh */
    const TapiocaSunStudyTransparency* transparency;
    size_t transparencyCount; /* material indices must be unique */
    double threshold;
    uint8_t glassOnly; /* 0 or 1 */
} TapiocaReceiversRequest;

typedef struct {
    size_t analysisFaces;
    size_t contextFaces;
    size_t unknownMaterialFaces;
    size_t counts[3]; /* analysis, context, ignored meshes after demotion */
    size_t maskCount; /* bytes written to faceMask */
} TapiocaReceiversSummary;

typedef struct {
    uint8_t* roles;   /* per mesh, after demotion */
    uint8_t* hasMask; /* per mesh: 1 mask populated, 0 every face */
    size_t meshCapacity;
    uint8_t* faceMask; /* concatenated per-mesh masks; zero where hasMask is 0 */
    size_t maskCapacity;
    TapiocaReceiversSummary* summary;
} TapiocaReceiversOutput;

/* BuildSunStudyReceivers over per-mesh triangle counts and materials. */
TapiocaStatus tapioca_sunstudy_receivers (const TapiocaReceiversRequest* request, const TapiocaReceiversOutput* output,
                                          char* error, size_t errorCapacity);

typedef struct {
    size_t triangleCount;
    TapiocaTextView preset;
} TapiocaPresetAdviceRequest;

typedef struct {
    char* text; /* UTF-8, not NUL-terminated */
    size_t capacity;
    size_t* length; /* 0 when there is no advice */
} TapiocaPresetAdviceOutput;

TapiocaStatus tapioca_sunstudy_preset_advice (const TapiocaPresetAdviceRequest* request,
                                              const TapiocaPresetAdviceOutput* output, char* error,
                                              size_t errorCapacity);

typedef struct {
    uint8_t present; /* 0: list not supplied (e.g. no study analysis list) */
    uint8_t bound;   /* the binding names a set for this role */
    TapiocaTextArrayView live;
    TapiocaTextArrayView study;
} TapiocaBoundList;

typedef struct {
    uint64_t generation; /* 0 = unbound */
    uint64_t currentGeneration;
    TapiocaBoundList lists[3]; /* context, ignored, analysis */
} TapiocaBindingRequest;

typedef struct {
    uint8_t refresh;    /* SelectionBindingRefresh value */
    uint8_t replace[3]; /* copy live into context, ignored, analysis */
} TapiocaBindingOutput;

/* The decision half of SunStudySelectionBinding::Refresh. */
TapiocaStatus tapioca_sunstudy_refresh_binding (const TapiocaBindingRequest* request, TapiocaBindingOutput* output,
                                                char* error, size_t errorCapacity);

/* Triangle-domain sampler. Empty groups / sampleGroup / sampleFace mean none. */
typedef struct {
    TapiocaDoubleView vertices;
    TapiocaIndexView triangles;
    TapiocaIndexView groups; /* one per face, or empty */
    TapiocaByteView sampleGroup;
    TapiocaByteView sampleFace;
    double spacing;
    double normalOffset;
    double jitter;
    size_t maxSamples;
    uint32_t jitterFaceOffset;
    uint8_t wantLayouts; /* 0 or 1 */
} TapiocaSampleGridRequest;

typedef struct {
    double origin[3];
    double uAxis[3];
    double vAxis[3];
    int64_t uStart; /* FaceLayout::uStart is `long` (32-bit on MSVC) */
    int64_t vStart;
    uint32_t columns;
    uint32_t rows;
    uint8_t gridded;
} TapiocaFaceLayout;

typedef struct {
    size_t sampleCount; /* 0 when refused */
    size_t faceCount;
    size_t degenerateFaces;
    size_t excludedFaces;
    size_t undersizedFaces;
    uint8_t valid;
} TapiocaSampleGridSummary;

/* sampleCapacity == 0: summary only. Otherwise every per-sample array holds
 * sampleCount entries (xyz arrays three each); with layouts, the cell arrays
 * too and `layouts` holds faceCount records. Each call samples from scratch. */
typedef struct {
    double* positions;
    double* normals;
    double* areas;
    uint32_t* faces;
    uint32_t* groups;
    uint32_t* cellColumns;
    uint32_t* cellRows;
    size_t sampleCapacity;
    TapiocaFaceLayout* layouts;
    size_t layoutCapacity;
    TapiocaSampleGridSummary* summary;
} TapiocaSampleGridOutput;

TapiocaStatus tapioca_sunstudy_sample_grid (const TapiocaSampleGridRequest* request,
                                            const TapiocaSampleGridOutput* output, char* error, size_t errorCapacity);

/* Surface-patch sampler. Empty groups / sampleGroup / sampleFace mean none. */
typedef struct {
    TapiocaDoubleView vertices;
    TapiocaIndexView triangles;
    TapiocaIndexView groups;
    TapiocaTextArrayView elementOf; /* element GUID per group */
    TapiocaByteView sampleGroup;
    TapiocaByteView sampleFace;
    double spacing;
    double normalOffset;
    double coplanarDegrees;
    double planeToleranceM;
    size_t maxSamples;
} TapiocaPatchGridRequest;

/* Key element = elementOf[group], or "" for a group past its end. */
typedef struct {
    size_t first;
    size_t count;
    uint64_t plane;
    uint32_t occurrence;
    uint32_t group;
    uint32_t columns;
    uint32_t rows;
    double frame[4][3]; /* origin, uAxis, vAxis, normal */
    double bounds[2][3];
    double area;
} TapiocaPatchSpan;

typedef struct {
    size_t sampleCount;
    size_t spanCount;
    size_t faceCount; /* patchOfTriangle entries; 0 when refused */
    size_t degenerateFaces;
    size_t centroidPatches;
    size_t excludedPatches;
    uint8_t sampleLimitExceeded;
    uint8_t valid;
} TapiocaPatchGridSummary;

/* All capacities 0: summary only. Otherwise each array holds its count. */
typedef struct {
    double* positions;
    double* normals;
    double* areas;
    uint32_t* cellColumns;
    uint32_t* cellRows;
    uint32_t* spanOf;
    size_t sampleCapacity;
    TapiocaPatchSpan* spans;
    size_t spanCapacity;
    uint32_t* patchOfTriangle;
    size_t faceCapacity;
    TapiocaPatchGridSummary* summary;
} TapiocaPatchGridOutput;

TapiocaStatus tapioca_sunstudy_patch_grid (const TapiocaPatchGridRequest* request, const TapiocaPatchGridOutput* output,
                                           char* error, size_t errorCapacity);

/* ---- atlas family ------------------------------------------------------ */

typedef struct {
    const TapiocaFaceLayout* layouts; /* one per source face */
    size_t layoutCount;
    TapiocaIndexView faces; /* per sample */
    TapiocaIndexView cellColumns;
    TapiocaIndexView cellRows;
} TapiocaAtlasSamplesView;

typedef struct {
    uint32_t x, y, width, height; /* width 0: not placed */
} TapiocaAtlasTile;

typedef struct {
    size_t tileCount; /* tiles written: layouts, or 0 */
    size_t texelCount;
    size_t placedFaces;
    uint32_t width;
    uint32_t height;
    uint8_t valid;
} TapiocaAtlasSummary;

/* Capacities: one tile per layout, one texel per sample. */
typedef struct {
    TapiocaAtlasTile* tiles;
    size_t tileCapacity;
    uint32_t* texels;
    size_t texelCapacity;
    TapiocaAtlasSummary* summary;
} TapiocaAtlasOutput;

typedef struct {
    TapiocaAtlasSamplesView samples;
    uint32_t gutter;
    uint32_t maxDimension;
} TapiocaTriangleAtlasRequest;

TapiocaStatus tapioca_sunstudy_triangle_atlas (const TapiocaTriangleAtlasRequest* request,
                                               const TapiocaAtlasOutput* output, char* error, size_t errorCapacity);

/* A patch atlas crosses by value until the store moves to Rust. */
typedef struct {
    TapiocaTextView element;
    uint64_t plane;
    uint32_t occurrence;
} TapiocaPatchKeyView;

typedef struct {
    TapiocaPatchKeyView key;
    uint32_t rect[5]; /* x, y, width, height, generation */
} TapiocaPatchAllocationView;

typedef struct {
    TapiocaPatchKeyView key;
    uint32_t columns;
    uint32_t rows;
} TapiocaPatchShapeView;

typedef struct {
    uint32_t fields[6]; /* width, height, penX, penY, shelfHeight, nextGeneration */
} TapiocaPatchAtlasCursor;

typedef struct {
    TapiocaPatchAtlasCursor cursor;
    const TapiocaPatchAllocationView* allocations; /* keys unique */
    size_t allocationCount;
} TapiocaPatchAtlasStateView;

/* element = text[elementOffset .. elementOffset + elementLength] */
typedef struct {
    size_t elementOffset;
    size_t elementLength;
    uint64_t plane;
    uint32_t occurrence;
    uint32_t rect[5];
} TapiocaPatchAllocation;

typedef struct {
    TapiocaPatchAtlasCursor* cursor;
    TapiocaPatchAllocation* allocations; /* key order */
    size_t allocationCapacity;           /* the shape/triangle count suffices */
    size_t* allocationCount;
    char* text; /* deduplicated element GUIDs */
    size_t textCapacity;
    size_t* textLength;
} TapiocaPatchAtlasStateOutput;

typedef struct {
    size_t counts[5]; /* retained, reused, moved, added, removed */
    uint8_t resized;
} TapiocaPatchAtlasUpdate;

typedef struct {
    TapiocaPatchAtlasStateView state;
    const TapiocaPatchShapeView* shapes;
    size_t shapeCount;
    uint32_t gutter;
    uint32_t maxDimension;
} TapiocaPatchAtlasFitRequest;

typedef struct {
    TapiocaPatchAtlasStateOutput state;
    TapiocaPatchAtlasUpdate* update;
} TapiocaPatchAtlasFitOutput;

/* SunStudyPatchAtlas::Fit on a state passed by value. */
TapiocaStatus tapioca_sunstudy_patch_atlas_fit (const TapiocaPatchAtlasFitRequest* request,
                                                const TapiocaPatchAtlasFitOutput* output, char* error,
                                                size_t errorCapacity);

typedef struct {
    TapiocaTextView guid; /* any spelling */
    size_t triangleCount;
} TapiocaAtlasMeshView;

typedef struct {
    TapiocaAtlasSamplesView samples;
    const TapiocaAtlasMeshView* meshes;
    size_t meshCount;
    TapiocaPatchAtlasStateView allocations;
    TapiocaPatchAtlasStateView previous;
    uint8_t hasPrevious;
    uint32_t gutter;
    uint32_t maxDimension;
} TapiocaStableAtlasRequest;

typedef struct {
    TapiocaAtlasOutput atlas;
    TapiocaPatchAtlasStateOutput state;
} TapiocaStableAtlasOutput;

/* BuildStableTriangleAtlas on states passed by value. */
TapiocaStatus tapioca_sunstudy_stable_triangle_atlas (const TapiocaStableAtlasRequest* request,
                                                      const TapiocaStableAtlasOutput* output, char* error,
                                                      size_t errorCapacity);

typedef struct {
    TapiocaByteView stepBits; /* sample-major, LSB-first, stepCount bits per sample */
    const int64_t* texelOfSample;
    size_t texelOfSampleCount;
    size_t sampleCount;
    size_t stepCount;
    uint32_t width;
    uint32_t height;
} TapiocaStepMaskRequest;

typedef struct {
    uint32_t* masks; /* words * width * height, slice-major */
    size_t capacity;
    uint32_t* words;
} TapiocaStepMaskOutput;

TapiocaStatus tapioca_sunstudy_step_masks (const TapiocaStepMaskRequest* request, const TapiocaStepMaskOutput* output,
                                           char* error, size_t errorCapacity);

typedef struct {
    uint32_t x, y, width, height, layer;
} TapiocaAtlasRegion;

/* f32 and u32 images both compare bytes, so both pass as 32-bit texels. */
typedef struct {
    const uint32_t* before;
    size_t beforeCount;
    const uint32_t* after;
    size_t afterCount;
    uint32_t size[3]; /* width, height, layers */
    size_t maxRegionsPerLayer;
} TapiocaTextureDeltaRequest;

typedef struct {
    TapiocaAtlasRegion* regions; /* layers * max(1, maxRegionsPerLayer) suffices */
    size_t capacity;
    size_t* count;
} TapiocaTextureDeltaOutput;

TapiocaStatus tapioca_sunstudy_texture_delta (const TapiocaTextureDeltaRequest* request,
                                              const TapiocaTextureDeltaOutput* output, char* error,
                                              size_t errorCapacity);

typedef struct {
    TapiocaSunStepView steps; /* a filtered series' kept steps */
    double fanIntervalMinutes;
} TapiocaStepScheduleRequest;

typedef struct {
    uint16_t* minutes; /* one per step */
    uint32_t* fan;     /* at most one per step */
    size_t capacity;
    size_t* fanCount;
    uint32_t* solarNoon;
} TapiocaStepScheduleOutput;

/* StepMinutes, SolarNoonStep and FanSteps. */
TapiocaStatus tapioca_sunstudy_step_schedule (const TapiocaStepScheduleRequest* request,
                                              const TapiocaStepScheduleOutput* output, char* error,
                                              size_t errorCapacity);

/* ---- occlusion accumulator (Rust-owned handle) ------------------------- */

/* ITraversal as callbacks. Callbacks must not throw, run on the calling thread
 * during the call only, and `context` must outlive the call. */
typedef struct {
    void* context;
    /* 1 complete, 0 cancelled (nothing committed), 2 failed. */
    uint8_t (*occludeDirectional) (void* context, const double* origins, size_t count, const double* direction,
                                   double tmin, double tmax, uint8_t* out, size_t maxParallel);
    uint64_t (*sceneVersion) (void* context);
    uint8_t (*isCancelled) (void* context); /* nullable; 1 cancelled */
} TapiocaTraversalVtable;

/* One thread at a time per handle; destroy after a TAPIOCA_ERR_INTERNAL. */
typedef struct TapiocaSunStudyOcclusion TapiocaSunStudyOcclusion;

TapiocaStatus tapioca_sunstudy_occlusion_create (size_t sampleCount, size_t stepCount, TapiocaSunStudyOcclusion** out,
                                                 char* error, size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_occlusion_destroy (TapiocaSunStudyOcclusion* handle, char* error, size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_occlusion_reset (TapiocaSunStudyOcclusion* handle, char* error, size_t errorCapacity);

typedef struct {
    const TapiocaSunStudyOcclusion* source;
    const size_t* map; /* source sample per sample; SIZE_MAX traces afresh */
    size_t mapCount;
    const uint64_t* dirty; /* optional sample-major dirty-step words */
    size_t dirtyCount;
} TapiocaOcclusionSeedRequest;

TapiocaStatus tapioca_sunstudy_occlusion_seed (TapiocaSunStudyOcclusion* handle,
                                               const TapiocaOcclusionSeedRequest* request, uint8_t* accepted,
                                               char* error, size_t errorCapacity);

typedef struct {
    const TapiocaTraversalVtable* traversal;
    TapiocaDoubleView positions;
    TapiocaDoubleView normals; /* empty: every sample faces the sun */
    size_t sampleCount;
    double tmin;
    double tmax;
    size_t maxParallel;
} TapiocaOcclusionTrace;

typedef struct {
    TapiocaOcclusionTrace trace;
    size_t step;
    double sun[3];
} TapiocaOcclusionStepRequest;

/* outcome: 0 committed, 1 refused, 2 cancelled. A failed traversal returns
 * TAPIOCA_ERR_STATE. */
TapiocaStatus tapioca_sunstudy_occlusion_step (TapiocaSunStudyOcclusion* handle,
                                               const TapiocaOcclusionStepRequest* request, uint8_t* outcome,
                                               char* error, size_t errorCapacity);

typedef struct {
    TapiocaOcclusionTrace trace;
    TapiocaDoubleView directions; /* xyz per series step */
    size_t first;
    size_t maxSteps;
} TapiocaOcclusionRangeRequest;

TapiocaStatus tapioca_sunstudy_occlusion_range (TapiocaSunStudyOcclusion* handle,
                                                const TapiocaOcclusionRangeRequest* request, size_t* done, char* error,
                                                size_t errorCapacity);

typedef struct {
    size_t sampleCount;
    size_t stepCount;
    size_t wordsPerSample;
    size_t resolvedSteps;
    size_t activeSamples;
    size_t lastRayCount;
    double compactMs;
    double traceMs;
    uint8_t complete;
} TapiocaOcclusionState;

TapiocaStatus tapioca_sunstudy_occlusion_state (TapiocaSunStudyOcclusion* handle, TapiocaOcclusionState* out,
                                                char* error, size_t errorCapacity);

/* Each array may be NULL with capacity 0 to skip it. */
typedef struct {
    uint64_t* bits; /* sampleCount * wordsPerSample */
    size_t bitCapacity;
    uint8_t* stepsResolved;
    size_t stepCapacity;
    double* sunHours;
    size_t hoursCapacity;
    double hoursPerStep;
} TapiocaOcclusionReadOutput;

TapiocaStatus tapioca_sunstudy_occlusion_read (TapiocaSunStudyOcclusion* handle,
                                               const TapiocaOcclusionReadOutput* output, char* error,
                                               size_t errorCapacity);

/* ---- study session (Rust-owned, internally synchronised) --------------- */

/* May be shared across threads: a concurrent advance resolves 0 steps, other
 * calls wait. Only destroy needs exclusivity. A poisoned session (after a
 * contained panic) returns TAPIOCA_ERR_STATE; destroy it. */
typedef struct TapiocaSunStudySession TapiocaSunStudySession;

TapiocaStatus tapioca_sunstudy_session_create (TapiocaSunStudySession** out, char* error, size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_session_destroy (TapiocaSunStudySession* handle, char* error, size_t errorCapacity);

typedef struct {
    uint64_t versions[3];     /* geometry, sun, grid; 0 = no such input */
    TapiocaSunStepView steps; /* kept steps, used as they are */
    int32_t timestepMinutes;
    size_t sourceStepCount;
    uint64_t seriesVersion;
    size_t sampleCount;
} TapiocaSessionSyncRequest;

TapiocaStatus tapioca_sunstudy_session_sync (const TapiocaSunStudySession* handle,
                                             const TapiocaSessionSyncRequest* request, char* error,
                                             size_t errorCapacity);

typedef struct {
    const TapiocaSunStudySession* source;
    const size_t* map; /* SIZE_MAX traces afresh */
    size_t mapCount;
    const uint64_t* dirty;
    size_t dirtyCount;
} TapiocaSessionSeedRequest;

TapiocaStatus tapioca_sunstudy_session_seed (const TapiocaSunStudySession* handle,
                                             const TapiocaSessionSeedRequest* request, uint8_t* accepted, char* error,
                                             size_t errorCapacity);

/* Runs on the advancing thread with the session locked: must not throw or
 * call into the session. */
typedef struct {
    void* context;
    void (*observe) (void* context, size_t step, double wallMs, const TapiocaOcclusionState* state); /* nullable */
} TapiocaStepObserver;

typedef struct {
    TapiocaOcclusionTrace trace; /* sampleCount must match the last sync */
    size_t maxSteps;
    TapiocaStepObserver observer;
} TapiocaSessionAdvanceRequest;

TapiocaStatus tapioca_sunstudy_session_advance (const TapiocaSunStudySession* handle,
                                                const TapiocaSessionAdvanceRequest* request, size_t* resolved,
                                                char* error, size_t errorCapacity);

typedef struct {
    uint64_t generation;
    size_t resolvedSteps;
    size_t totalSteps;
    size_t sampleCount;
    TapiocaOcclusionState accumulator;
    uint8_t converged;
    uint8_t empty;
    uint8_t advancing;
} TapiocaSessionProgress;

TapiocaStatus tapioca_sunstudy_session_progress (const TapiocaSunStudySession* handle, TapiocaSessionProgress* out,
                                                 char* error, size_t errorCapacity);

/* hoursPerStep is ignored: hours use the session's own day. */
TapiocaStatus tapioca_sunstudy_session_read (const TapiocaSunStudySession* handle,
                                             const TapiocaOcclusionReadOutput* output, char* error,
                                             size_t errorCapacity);

/* ---- reuse planning, patch sample maps, result summary ----------------- */

typedef struct {
    TapiocaTextView guid;
    TapiocaDoubleView vertices;
    TapiocaIndexView triangles;
} TapiocaReuseMeshView;

typedef struct {
    TapiocaSunStepView steps; /* kept steps, used as they are */
    int32_t timestepMinutes;
    size_t sourceStepCount;
    uint64_t version;
} TapiocaSeriesView;

typedef struct {
    const TapiocaReuseMeshView* meshes;
    size_t meshCount;
    TapiocaByteView roles; /* one per mesh */
    TapiocaDoubleView positions;
    TapiocaDoubleView normals;
    TapiocaIndexView sampleMeshes; /* mesh index per sample */
    TapiocaSeriesView series;
    double gridSpacing;
    uint8_t domain; /* SamplingDomain */
} TapiocaReuseStudyView;

typedef struct {
    void* context;
    uint8_t (*isCancelled) (void* context); /* nullable; must not throw */
} TapiocaCancelCallback;

typedef struct {
    TapiocaReuseStudyView source;
    TapiocaReuseStudyView target;
    TapiocaCancelCallback cancel;
    uint8_t sourceConverged;
    uint8_t sourceDefaultRayBounds;
} TapiocaReusePlanRequest;

typedef struct {
    size_t reusedSamples;
    size_t reusedSampleSteps;
    uint8_t planned; /* 0: no reuse, outputs untouched */
} TapiocaReusePlanSummary;

typedef struct {
    size_t* map; /* one per target sample */
    size_t mapCapacity;
    uint64_t* dirty; /* target samples x ceil(steps / 64) */
    size_t dirtyCapacity;
    TapiocaReusePlanSummary* summary;
} TapiocaReusePlanOutput;

/* The planning half of ReuseUnaffectedSamples; the caller seeds the session. */
TapiocaStatus tapioca_sunstudy_plan_reuse (const TapiocaReusePlanRequest* request, const TapiocaReusePlanOutput* output,
                                           char* error, size_t errorCapacity);

typedef struct {
    TapiocaTextView element;
    size_t first;
    size_t count;
} TapiocaSpanOwnerView;

typedef struct {
    const TapiocaReuseMeshView* meshes;
    size_t meshCount;
    const TapiocaSpanOwnerView* spans;
    size_t spanCount;
    size_t sampleCount;
} TapiocaSampleMeshRequest;

typedef struct {
    uint32_t* meshes; /* written only when outcome is 0 */
    size_t capacity;
    uint8_t* outcome; /* 0 written, 1 ambiguous (keep), 2 bad span (clear) */
} TapiocaSampleMeshOutput;

TapiocaStatus tapioca_sunstudy_patch_sample_meshes (const TapiocaSampleMeshRequest* request,
                                                    const TapiocaSampleMeshOutput* output, char* error,
                                                    size_t errorCapacity);

typedef struct {
    TapiocaDoubleView hours;
    double daylightHours;
} TapiocaSummaryRequest;

typedef struct {
    size_t count;
    size_t fullyLit;
    size_t fullyShaded;
    double minHours;
    double meanHours;
    double maxHours;
    double daylightHours;
} TapiocaResultSummary;

TapiocaStatus tapioca_sunstudy_summarize (const TapiocaSummaryRequest* request, TapiocaResultSummary* out, char* error,
                                          size_t errorCapacity);

/* ---- surface sampling ------------------------------------------------- */

typedef struct TapiocaSurfaceResult TapiocaSurfaceResult;

typedef struct {
    double spacing;
    double normalOffset;
    double jitter;
    size_t maxSamples;
    uint8_t domain; /* SamplingDomain: 0 triangle, 1 patch */
} TapiocaSurfaceOptions;

/* One mesh's layout span. On input sampleFaces is that mesh's mask; on output
 * sampleFaces.data is null, sampleFaces.count its length and maskOffset its
 * start in the read buffer. */
typedef struct {
    size_t ranges[9];  /* firstFace, faceCount, firstSample, sampleCount, firstPatch,
                          patchCount, degenerate, undersized, excluded */
    size_t winding[4]; /* groups, closed, flipped, flippedTriangles */
    TapiocaByteView sampleFaces;
    size_t maskOffset;
    uint8_t sampled;
} TapiocaMeshSpan;

typedef struct {
    const TapiocaMeshSpan* data;
    size_t count;
} TapiocaMeshSpanView;

typedef struct {
    const TapiocaFaceLayout* data;
    size_t count;
} TapiocaFaceLayoutView;

typedef struct {
    TapiocaDoubleView positions;
    TapiocaDoubleView normals;
    TapiocaDoubleView areas;
    TapiocaIndexView faces;
    TapiocaIndexView groups;
    TapiocaFaceLayoutView layouts;
    TapiocaIndexView cellColumns;
    TapiocaIndexView cellRows;
    size_t counters[3]; /* degenerate, excluded, undersized */
    uint8_t valid;
} TapiocaSampleGridView;

typedef struct {
    TapiocaPatchSpan record;
    TapiocaTextView element;
} TapiocaPatchSpanEntry;

typedef struct {
    const TapiocaPatchSpanEntry* data;
    size_t count;
} TapiocaPatchSpanEntryView;

typedef struct {
    TapiocaDoubleView positions;
    TapiocaDoubleView normals;
    TapiocaDoubleView areas;
    TapiocaIndexView cellColumns;
    TapiocaIndexView cellRows;
    TapiocaIndexView spanOf;
    TapiocaPatchSpanEntryView spans;
    TapiocaIndexView patchOfTriangle;
    size_t counters[3]; /* degenerateFaces, centroidPatches, excludedPatches */
    uint8_t sampleLimitExceeded;
    uint8_t valid;
} TapiocaPatchGridView;

typedef struct {
    const TapiocaReuseMeshView* data;
    size_t count;
} TapiocaReuseMeshArray;

typedef struct {
    TapiocaReuseMeshArray meshes;
    TapiocaSurfaceOptions options;
    TapiocaMeshSpanView spans;
    TapiocaSampleGridView sampleGrid;
    TapiocaPatchGridView patchGrid;
    uint8_t layoutValid;
    uint8_t domain;
} TapiocaPreviousSampling;

typedef struct {
    const TapiocaByteView* data;
    size_t count;
} TapiocaByteViewArray;

typedef struct {
    TapiocaReuseMeshArray meshes;
    TapiocaByteView sampleMask;     /* one per mesh */
    TapiocaByteViewArray faceMasks; /* empty: none; an empty entry: every face */
    TapiocaSurfaceOptions options;
    TapiocaPreviousSampling previous; /* read only when hasPrevious */
    TapiocaCancelCallback cancel;
    uint8_t hasPrevious;
} TapiocaSurfaceBuildRequest;

/* BuildSurfaceSampling. The result is an immutable Rust-owned handle that any
 * thread may read; destroy it exactly once. */
TapiocaStatus tapioca_sunstudy_surface_build (const TapiocaSurfaceBuildRequest* request, TapiocaSurfaceResult** out,
                                              char* error, size_t errorCapacity);

TapiocaStatus tapioca_sunstudy_surface_destroy (TapiocaSurfaceResult* handle, char* error, size_t errorCapacity);

typedef struct {
    size_t sizes[8];   /* triangle samples, triangle layouts, patch samples, patch spans,
                          patchOfTriangle, layout meshes, mask bytes, span text bytes */
    size_t counts[4];  /* reusedMeshes, rebuiltMeshes, reusedSamples, generatedSamples */
    size_t winding[4]; /* summed WindingReport */
    size_t triangleCounters[3];
    size_t patchCounters[3];
    TapiocaSurfaceOptions options; /* the layout's */
    uint8_t flags[5];              /* result, triangle grid, patch grid, layout valid; sample limit */
} TapiocaSurfaceResultSummary;

TapiocaStatus tapioca_sunstudy_surface_summary (const TapiocaSurfaceResult* handle, TapiocaSurfaceResultSummary* out,
                                                char* error, size_t errorCapacity);

/* Each destination holds the size the summary reported (xyz: three per
 * sample); null only where that size is zero. */
typedef struct {
    double* triangleReals[3];     /* positions, normals, areas */
    uint32_t* triangleIndices[4]; /* faces, groups, cellColumns, cellRows */
    TapiocaFaceLayout* triangleLayouts;
    double* patchReals[3];     /* positions, normals, areas */
    uint32_t* patchIndices[4]; /* cellColumns, cellRows, spanOf, patchOfTriangle */
    TapiocaPatchSpan* spans;
    size_t* spanTextOffsets; /* element of span i starts here in text */
    char* text;
    TapiocaMeshSpan* meshSpans;
    uint8_t* masks;
} TapiocaSurfaceReadOutput;

TapiocaStatus tapioca_sunstudy_surface_read (const TapiocaSurfaceResult* handle, const TapiocaSurfaceReadOutput* output,
                                             char* error, size_t errorCapacity);

/* ---- study core (the portable half of a split StudyRecord) ------------ */

/* create, setSampleMeshes and reuse need exclusive use (the study is still
 * being prepared); afterwards any thread may call with a shared handle: a
 * concurrent advance resolves 0 steps, other calls wait. Only destroy needs
 * exclusivity again. A poisoned session returns TAPIOCA_ERR_STATE. */
typedef struct TapiocaSunStudyStudy TapiocaSunStudyStudy;

typedef struct {
    const TapiocaAtlasMeshView* data;
    size_t count;
} TapiocaAtlasMeshArray;

/* Only the arrays of `domain` are read. */
typedef struct {
    uint8_t domain; /* 0 ground, 1 triangle, 2 patch */
    TapiocaSeriesView series;
    double spacing;
    TapiocaDoubleView positions;          /* ground */
    TapiocaDoubleView normals;            /* ground; empty: always facing */
    TapiocaSampleGridView sampleGrid;     /* triangle, with layouts */
    TapiocaAtlasMeshArray meshes;         /* triangle: stable tile identities */
    uint32_t atlas[2];                    /* gutter (triangle), maxDimension */
    TapiocaPatchGridView patchGrid;       /* patch */
    const TapiocaSunStudyStudy* previous; /* the study being rerun, or null */
} TapiocaStudyCreateRequest;

/* outcome: 0 created (out written), 1 inconsistent samples, 2 patch atlas
 * refused. */
TapiocaStatus tapioca_sunstudy_study_create (const TapiocaStudyCreateRequest* request, TapiocaSunStudyStudy** out,
                                             uint8_t* outcome, char* error, size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_study_destroy (TapiocaSunStudyStudy* handle, char* error, size_t errorCapacity);

/* geometry, sun, grid; 0 = no such input */
TapiocaStatus tapioca_sunstudy_study_sync (const TapiocaSunStudyStudy* handle, const uint64_t (*versions)[3],
                                           char* error, size_t errorCapacity);

typedef struct {
    TapiocaReuseMeshArray meshes;
} TapiocaStudyMeshesView;

TapiocaStatus tapioca_sunstudy_study_set_sample_meshes (TapiocaSunStudyStudy* handle,
                                                        const TapiocaStudyMeshesView* view, char* error,
                                                        size_t errorCapacity);

typedef struct {
    const TapiocaSunStudyStudy* source; /* converged, not the target */
    TapiocaReuseMeshArray sourceMeshes;
    TapiocaByteView sourceRoles;
    TapiocaReuseMeshArray targetMeshes;
    TapiocaByteView targetRoles;
    uint8_t sourceDefaultRayBounds;
    TapiocaCancelCallback cancel;
} TapiocaStudyReuseRequest;

typedef struct {
    size_t reusedSamples;
    size_t reusedSampleSteps;
    uint8_t seeded; /* 0: nothing seeded */
} TapiocaStudyReuseOutcome;

TapiocaStatus tapioca_sunstudy_study_reuse (TapiocaSunStudyStudy* handle, const TapiocaStudyReuseRequest* request,
                                            TapiocaStudyReuseOutcome* out, char* error, size_t errorCapacity);

typedef struct {
    const TapiocaTraversalVtable* traversal;
    double tmin;
    double tmax;
    size_t maxParallel;
    size_t maxSteps;
    TapiocaStepObserver observer;
} TapiocaStudyAdvanceRequest;

TapiocaStatus tapioca_sunstudy_study_advance (const TapiocaSunStudyStudy* handle,
                                              const TapiocaStudyAdvanceRequest* request, size_t* resolved, char* error,
                                              size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_study_progress (const TapiocaSunStudyStudy* handle, TapiocaSessionProgress* out,
                                               char* error, size_t errorCapacity);

/* Domain read statuses: 0 ok, 1 no triangle (model-surface) atlas, 2 no patch
 * atlas, 3 a patch the atlas did not place. A refusal writes no array. */
typedef struct {
    size_t sampleCount;
    size_t stepCount;
    size_t sampleMeshCount;
    size_t reuse[2]; /* reusedSamples, reusedSampleSteps */
    size_t displayFaces;
    double spacing;
    double daylightHours;
    double analysedArea;
    uint32_t displaySize[2];
    uint8_t domain;
    uint8_t displayStatus;
} TapiocaStudyInfo;

TapiocaStatus tapioca_sunstudy_study_info (const TapiocaSunStudyStudy* handle, TapiocaStudyInfo* out, char* error,
                                           size_t errorCapacity);

/* Capacity 0 skips an array; otherwise it must hold the whole array. */
typedef struct {
    double* hours;
    size_t hoursCapacity;
    uint8_t* stepBits; /* one byte per (sample, step), sample-major */
    size_t stepBitsCapacity;
    uint32_t* sampleMeshes;
    size_t sampleMeshesCapacity;
} TapiocaStudyArraysOutput;

TapiocaStatus tapioca_sunstudy_study_arrays (const TapiocaSunStudyStudy* handle, const TapiocaStudyArraysOutput* output,
                                             char* error, size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_study_summary (const TapiocaSunStudyStudy* handle, TapiocaResultSummary* out,
                                              char* error, size_t errorCapacity);

/* Shape-then-fill: zero capacities report the sizes only. */
typedef struct {
    uint8_t kind; /* 0 triangle image, 1 patch image, 2 display data */
    float* image;
    size_t imageCapacity;
    TapiocaAtlasTile* tiles;    /* display data */
    TapiocaFaceLayout* layouts; /* display data */
    size_t faceCapacity;
} TapiocaStudyImageOutput;

typedef struct {
    size_t texels;
    size_t faces;
    uint32_t width;
    uint32_t height;
    uint8_t status;
} TapiocaStudyImageSummary;

TapiocaStatus tapioca_sunstudy_study_image (const TapiocaSunStudyStudy* handle, const TapiocaStudyImageOutput* output,
                                            TapiocaStudyImageSummary* summary, char* error, size_t errorCapacity);

typedef struct {
    uint32_t* masks;
    size_t maskCapacity;
    uint16_t* minutes;
    size_t minuteCapacity;
} TapiocaStudyStepMaskOutput;

typedef struct {
    size_t maskCount;
    size_t minuteCount;
    uint32_t shape[5]; /* width, height, words, steps, solar-noon step */
    uint8_t status;
} TapiocaStudyStepMaskSummary;

TapiocaStatus tapioca_sunstudy_study_step_masks (const TapiocaSunStudyStudy* handle,
                                                 const TapiocaStudyStepMaskOutput* output,
                                                 TapiocaStudyStepMaskSummary* summary, char* error,
                                                 size_t errorCapacity);

typedef struct {
    size_t face;
    double point[3];
} TapiocaStudyReadAtRequest;

typedef struct {
    double hours;
    size_t sample;
    uint32_t cell[2]; /* column, row */
    uint8_t measured;
    uint8_t nearest;
} TapiocaStudyReading;

TapiocaStatus tapioca_sunstudy_study_read_at (const TapiocaSunStudyStudy* handle,
                                              const TapiocaStudyReadAtRequest* request, TapiocaStudyReading* out,
                                              char* error, size_t errorCapacity);

/* ---- follower policy (Rust-owned; one thread at a time) ---------------- */

typedef struct TapiocaSunStudyFollower TapiocaSunStudyFollower;

TapiocaStatus tapioca_sunstudy_follower_create (TapiocaSunStudyFollower** out, char* error, size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_follower_destroy (TapiocaSunStudyFollower* handle, char* error, size_t errorCapacity);

typedef struct {
    uint8_t kind;          /* 0 adopt, 1 observe, 2 shouldStart, 3 msUntilStart, 4 noteStarted, 5 canPublish,
                              6 noteCompleted, 7 noteFailed, 8 clear, 9 setDebounce, 10 read only */
    uint64_t signature[3]; /* geometry, sun, sampling */
    int64_t nowMs;
    uint64_t generation;
    int64_t debounceMs;
    TapiocaTextView studyId;
} TapiocaFollowerEvent;

typedef struct {
    int64_t answer; /* flag 0/1, generation bits, or milliseconds */
    uint64_t study[3];
    uint64_t world[3];
    uint64_t counters[3]; /* generation, reruns, discarded */
    int64_t debounceMs;
    uint8_t state;       /* SunStudyFollowState */
    uint8_t dirtyReason; /* SunStudyDirtyReason */
} TapiocaFollowerSnapshot;

TapiocaStatus tapioca_sunstudy_follower_apply (TapiocaSunStudyFollower* handle, const TapiocaFollowerEvent* event,
                                               TapiocaFollowerSnapshot* out, char* error, size_t errorCapacity);

typedef struct {
    uint8_t which; /* 0 study id, 1 description */
    char* text;
    size_t capacity;
    size_t* length; /* written even when the text did not fit */
} TapiocaFollowerTextOutput;

TapiocaStatus tapioca_sunstudy_follower_text (TapiocaSunStudyFollower* handle, const TapiocaFollowerTextOutput* output,
                                              char* error, size_t errorCapacity);

/* ---- visibility and viewpoint studies (policy in Rust) ---------------- */

typedef struct {
    double origin[3];
    double dir[3]; /* unit */
    double length;
} TapiocaFirstHitRay;

typedef struct {
    double t;
    uint32_t mesh; /* snapshot mesh index */
    uint8_t hit;
} TapiocaRayHit;

typedef struct {
    double origin[3];
    double dir[3];
    double tmin;
    double tmax;
} TapiocaBoundedRay;

/* The host scene index. Callbacks return 1 done, 0 cancelled, 2 failed and
 * must not throw; a null mask is the whole model. */
typedef struct {
    void* context;
    uint8_t (*prepare) (void* context); /* nullable */
    uint8_t (*firstHits) (void* context, const TapiocaFirstHitRay* rays, size_t count, const uint8_t* mask,
                          size_t maskCount, TapiocaRayHit* out, size_t maxParallel);
    uint8_t (*occludeRays) (void* context, const TapiocaBoundedRay* rays, size_t count, uint8_t* out,
                            size_t maxParallel);
    uint8_t (*isCancelled) (void* context); /* nullable */
} TapiocaRayQueryVtable;

typedef struct {
    TapiocaReuseMeshArray meshes; /* only the GUIDs are read */
    TapiocaTextArrayView context;
    double point[3];
    double radius;
    size_t rays;
    const TapiocaRayQueryVtable* queries;
    double angles[4]; /* bearing, field of view, min and max elevation; degrees */
    size_t grid[2];   /* sphere columns, rows */
    uint8_t mode;     /* ViewpointMode */
} TapiocaViewpointRequest;

typedef struct {
    double (*perimeter)[3]; /* one per ray: plan rays, sector rays, or sphere rows x columns */
    size_t capacity;
    size_t (*counts)[2];  /* points, collisions */
    double (*measures)[3]; /* area, volume, openFraction */
    int64_t* rayMesh;      /* capacity entries: stopping mesh index, or -1 */
} TapiocaViewpointOutput;

/* RunViewpointStudy without the sphere's seen surfaces (the host's pass over its
 * own triangles); a refusal returns TAPIOCA_ERR_STATE with its message. */
TapiocaStatus tapioca_sunstudy_viewpoint (const TapiocaViewpointRequest* request, const TapiocaViewpointOutput* output,
                                          char* error, size_t errorCapacity);

typedef struct {
    double reals[6];  /* spacing, normalOffset, tmin, coneDegrees, min and max elevation degrees */
    size_t limits[4]; /* maxSamples, maxAimPoints, maxRays, maxParallel */
    double point[3];
    double direction[3];
    uint8_t origin; /* VisibilityOrigin */
    uint8_t domain; /* SamplingDomain */
    uint8_t explicitContext;
    uint8_t view; /* VisibilityView */
} TapiocaVisibilityOptions;

typedef struct {
    TapiocaReuseMeshArray meshes;
    TapiocaTextArrayView roles[3]; /* from, to, context */
    TapiocaVisibilityOptions options;
    const TapiocaRayQueryVtable* queries;
} TapiocaVisibilityRequest;

typedef struct TapiocaVisibilityResult TapiocaVisibilityResult;

/* RunVisibilityStudy; a refusal returns TAPIOCA_ERR_STATE with its message. */
TapiocaStatus tapioca_sunstudy_visibility_run (const TapiocaVisibilityRequest* request, TapiocaVisibilityResult** out,
                                               char* error, size_t errorCapacity);
TapiocaStatus tapioca_sunstudy_visibility_destroy (TapiocaVisibilityResult* handle, char* error, size_t errorCapacity);

typedef struct {
    size_t sizes[8]; /* values, aims, rays, visible, triangle tiles, triangle texels,
                        patch allocations, patch allocation text bytes */
    double reals[2]; /* meanVisibility, analysedArea */
    uint8_t domain;
} TapiocaVisibilitySummary;

TapiocaStatus tapioca_sunstudy_visibility_summary (const TapiocaVisibilityResult* handle, TapiocaVisibilitySummary* out,
                                                   char* error, size_t errorCapacity);

typedef struct {
    double* values;
    size_t valueCapacity;
    TapiocaAtlasOutput atlas;
    TapiocaPatchAtlasStateOutput patchAtlas;
} TapiocaVisibilityReadOutput;

TapiocaStatus tapioca_sunstudy_visibility_read (const TapiocaVisibilityResult* handle,
                                                const TapiocaVisibilityReadOutput* output, char* error,
                                                size_t errorCapacity);

/* The result's grids as a surface result (read and destroy as one). */
TapiocaStatus tapioca_sunstudy_visibility_grids (const TapiocaVisibilityResult* handle, TapiocaSurfaceResult** out,
                                                 char* error, size_t errorCapacity);

/* The sun-study surface's own version, independent of TAPIOCA_ABI_VERSION. */
TapiocaStatus tapioca_sunstudy_version (uint32_t* version, char* error, size_t errorCapacity);

#ifdef __cplusplus
}
#endif

#endif
