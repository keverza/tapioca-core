// DxrWorker/DxrOcclusion.hlsl -- the DXR worker's one question: is anything
// between this sample and the sun.
//
// Compiled ahead of time to DXIL (lib_6_3, DXR Tier 1.0) by
// compile-dxr-shaders.ps1 into DxrOcclusionDxil.h, which is committed, so an
// ordinary build needs no shader compiler. Edit this file, rerun the script,
// commit both.
//
// ⚠️ THE CPU QUERY IS THE CONTRACT (QueryEngine::Occluded): two-sided (no
// back-face culling), every triangle opaque, ANY hit inside [tmin, tmax] means
// occluded. Hence FORCE_OPAQUE and no cull flag; the first accepted hit ends
// the search and no closest-hit shader runs. Back-facing samples never reach
// here: the CPU decides incidence in double, exactly as the accumulator does,
// and only front-facing (sample, step) pairs are dispatched.

RaytracingAccelerationStructure g_scene : register (t0);
// Sample positions in the scene's local frame (f64 world minus the shared
// origin, then f32), xyz per sample.
StructuredBuffer<float3> g_positions : register (t1);
// One ray per entry: x = sample index, y = step slot in g_directions.
StructuredBuffer<uint2> g_rays : register (t2);
// Unit sun directions (normalised in double on the CPU), one per step slot.
StructuredBuffer<float3> g_directions : register (t3);
// One word per ray: 1 occluded, 0 clear.
RWStructuredBuffer<uint> g_occluded : register (u0);

cbuffer Launch : register (b0)
{
    float g_tmin;
    float g_tmax;
    uint g_rayBase;  // first ray of this launch in g_rays / g_occluded
    uint g_rayCount; // rays in this launch
};

struct Payload {
    uint occluded;
};

[shader ("raygeneration")] void RayGen ()
{
    const uint i = DispatchRaysIndex ().x;
    if (i >= g_rayCount)
        return;
    const uint2 entry = g_rays[g_rayBase + i];

    RayDesc ray;
    ray.Origin = g_positions[entry.x];
    ray.Direction = g_directions[entry.y];
    ray.TMin = g_tmin;
    ray.TMax = g_tmax;

    // Occluded unless the miss shader says otherwise: an accepted hit ends the
    // search without running any further shader.
    Payload payload;
    payload.occluded = 1;
    TraceRay (g_scene,
              RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE,
              0xFF, 0, 1, 0, ray, payload);
    g_occluded[g_rayBase + i] = payload.occluded;
}

[shader ("miss")] void Miss (inout Payload payload)
{
    payload.occluded = 0;
}

// Never invoked (SKIP_CLOSEST_HIT_SHADER); present so the hit group record is
// a real one rather than relying on a null shader identifier.
[shader ("closesthit")] void ClosestHit (inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    payload.occluded = 1;
}
