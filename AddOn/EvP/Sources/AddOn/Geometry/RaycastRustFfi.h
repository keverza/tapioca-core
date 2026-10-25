#ifndef TAPIOCA_RAYCAST_RUST_FFI_H
#define TAPIOCA_RAYCAST_RUST_FFI_H

/* Ray-tracing scene ABI (raycast plan E0), part of the aggregate archive.
 * A scene is built from the C++ engine's concatenated arrays, owned by Rust
 * and immutable: any number of threads may query one scene at once; destroy
 * must not overlap a query. Statuses and views are GeometryRustFfi.h's. */

#include "Geometry/GeometryRustFfi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TapiocaRaycastScene TapiocaRaycastScene;

typedef struct {
    TapiocaDoubleView vertices; /* 3 * vertexCount world-metre coordinates */
    TapiocaIndexView triangles; /* 3 * triangleCount vertex indices */
    uint64_t version;           /* geometry identity (snapshot id) */
} TapiocaRaycastSceneRequest;

typedef struct {
    void* context;
    uint8_t (*isCancelled) (void* context); /* nullable; must not throw */
} TapiocaRaycastCancel;

typedef struct {
    TapiocaDoubleView origins;   /* 3 * count */
    double direction[3];         /* normalised per ray */
    double tmin;                 /* <= 0 starts at the origin */
    double tmax;                 /* <= 0 is unbounded */
    size_t maxParallel;          /* 0 decides, 1 inline, n at most n threads */
    TapiocaRaycastCancel cancel; /* polled on the calling thread only */
} TapiocaRaycastDirectionalRequest;

/* Validates (shape, finite coordinates, indices, < 2^31 triangles) and builds
 * the hierarchy. A refused scene is TAPIOCA_ERR_ARG with the reason. */
TapiocaStatus tapioca_raycast_scene_build (const TapiocaRaycastSceneRequest* request, TapiocaRaycastScene** out,
                                           char* error, size_t errorCapacity);
/* Null is ignored. */
TapiocaStatus tapioca_raycast_scene_destroy (TapiocaRaycastScene* scene, char* error, size_t errorCapacity);
/* QueryEngine::Occluded per origin into out (1 occluded, 0 clear);
 * completed = 0 when cancelled (out then partially written). */
TapiocaStatus tapioca_raycast_occlude_directional (const TapiocaRaycastScene* scene,
                                                   const TapiocaRaycastDirectionalRequest* request, uint8_t* out,
                                                   size_t outCount, uint8_t* completed, char* error,
                                                   size_t errorCapacity);

#ifdef __cplusplus
}

static_assert (sizeof (TapiocaRaycastSceneRequest) == 40, "Rust RaycastSceneRequest");
static_assert (sizeof (TapiocaRaycastDirectionalRequest) == 80, "Rust RaycastDirectionalRequest");
#endif

#endif
