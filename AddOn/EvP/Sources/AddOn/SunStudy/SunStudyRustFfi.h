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

/* The sun-study surface's own version, independent of TAPIOCA_ABI_VERSION. */
TapiocaStatus tapioca_sunstudy_version (uint32_t* version, char* error, size_t errorCapacity);

#ifdef __cplusplus
}
#endif

#endif
