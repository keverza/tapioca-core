// OptixWorker/OptixOcclusion.cu -- the OptiX worker's one question: is anything
// between this sample and the sun. The OptiX twin of DxrOcclusion.hlsl.
//
// Compiled at build time to OptiX-IR (nvcc -optix-ir) and embedded in the
// worker; nothing generated from the OptiX SDK is committed (decision 8).
//
// ⚠️ THE CPU QUERY IS THE CONTRACT (QueryEngine::Occluded): two-sided (no cull
// flag), every triangle opaque (DISABLE_ANYHIT), ANY hit inside [tmin, tmax]
// occludes, so the first hit terminates the search and no closest-hit runs.
// Back-facing samples never reach here: the CPU decides incidence in double.

#include <optix.h>

#include "OptixWorker/OptixLaunchParams.h"

extern "C" {
__constant__ evp::optixworker::LaunchParams params;
}

extern "C" __global__ void __raygen__occlusion ()
{
    const unsigned int i = optixGetLaunchIndex ().x;
    if (i >= params.rayCount)
        return;
    const uint2 entry = params.rays[params.rayBase + i];
    const float3 origin = params.positions[entry.x];
    const float3 direction = params.directions[entry.y];
    // Occluded unless the miss program says otherwise.
    unsigned int occluded = 1u;
    optixTrace (params.scene, origin, direction, params.tmin, params.tmax, 0.0f, OptixVisibilityMask (255),
                OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT |
                    OPTIX_RAY_FLAG_DISABLE_ANYHIT,
                0, 1, 0, occluded);
    params.occluded[params.rayBase + i] = occluded;
}

extern "C" __global__ void __miss__occlusion ()
{
    optixSetPayload_0 (0u);
}
