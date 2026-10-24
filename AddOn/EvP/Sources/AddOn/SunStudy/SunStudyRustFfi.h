#ifndef TAPIOCA_SUNSTUDY_RUST_FFI_H
#define TAPIOCA_SUNSTUDY_RUST_FFI_H

/* Sun-study C ABI v1, exported by the aggregate tapioca_core_ffi archive.
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

#define TAPIOCA_SUNSTUDY_ABI_VERSION UINT32_C (1)

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

/* The sun-study surface's own version, independent of TAPIOCA_ABI_VERSION. */
TapiocaStatus tapioca_sunstudy_version (uint32_t* version, char* error, size_t errorCapacity);

#ifdef __cplusplus
}
#endif

#endif
