#ifndef EVP_OPTIXWORKER_OPTIXLAUNCHPARAMS_H
#define EVP_OPTIXWORKER_OPTIXLAUNCHPARAMS_H

// OptixWorker/OptixLaunchParams.h -- the one struct the host fills and the
// device program reads (`params`). Shared verbatim by OptixOcclusion.cu and
// OptixBackend.cpp, so the layout cannot drift between them.

#include <optix_types.h>
#include <vector_types.h>

namespace evp::optixworker {

struct LaunchParams {
    OptixTraversableHandle scene;
    // Sample positions in the scene's local frame (f64 world minus the shared
    // origin, then f32).
    const float3* positions;
    // One ray per entry: x = sample index, y = step slot in `directions`.
    const uint2* rays;
    // Unit sun directions (normalised in double on the CPU), one per slot.
    const float3* directions;
    // One word per ray: 1 occluded, 0 clear.
    unsigned int* occluded;
    float tmin;
    float tmax;
    unsigned int rayBase;  // first ray of this launch
    unsigned int rayCount; // rays in this launch
};

} // namespace evp::optixworker

#endif
