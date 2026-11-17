#ifndef EVP_ARCHVIZ_RAYTRACEDSHADER_HPP
#define EVP_ARCHVIZ_RAYTRACEDSHADER_HPP

// ArchViz/RayTracedShader -- the ray-traced display style's one compute shader (issue #231).
//
// Inline ray tracing (RayQuery, Shader Model 6.5), compiled at runtime by the bundled DXC. One
// thread per pixel traces `count` samples into a running mean in g_accum, which the resolve then
// tone-maps exactly as it does the raster image.
//
// ⚠️ THE LIGHT IS THE RASTER PASS'S LIGHT, NOT A NEW ONE. Sun irradiance is the raster's direct
// weight times its sun strength; the sky is the environment map at the HUD's intensity when one is
// loaded (whose diffuse convolution IS the raster's SH ambient), and the raster's sky/ground
// hemisphere times the ambient share otherwise. Lambertian surfaces over the same resolved
// materials (clay, the study model and the presets already applied on the CPU). So the style
// differs from Rendered by what tracing adds -- exact shadows, interreflection, sky occlusion --
// and not by exposure.
//
// ⚠️ THE SECTION PLANE AND GLASS ARE CANDIDATE TESTS. Every triangle is non-opaque so each
// candidate passes through the loop: one beyond the plane is skipped (the clip), and one whose
// material is translucent is committed with probability alpha (stochastic transparency, which
// converges to the blend). A back face hit with the plane on is the cut: shaded as the cap colour
// lying in the plane, the same reading the raster's back-face pass gives.
//
// Constants mirror RayTracedConstants in DiligentSceneRayTraced.cpp field for field.

namespace geomsrv::archviz {

inline constexpr char kRayTracedCS[] = R"hlsl(
cbuffer RayTracedConstants
{
    float4x4 g_invViewProj;
    float4   g_sun;     // xyz toward the sun, w irradiance
    float4   g_sky;     // rgb sky colour, w ambient share (no environment map)
    float4   g_ground;  // rgb ground colour
    float4   g_env;     // x intensity, y rotation rad, z map loaded, w background drawn
    float4   g_plane;   // keep where dot(xyz, p) + w >= 0
    float4   g_cap;     // rgb cut colour (linear), w section on
    uint4    g_frame;   // x width, y height, z first sample, w bounces
    float4   g_ray;     // x near-plane NDC depth, y samples this dispatch
};

RaytracingAccelerationStructure g_scene;
StructuredBuffer<float3> g_positions;
StructuredBuffer<uint>   g_indices;
StructuredBuffer<uint>   g_triangleMaterial;
struct Material { float4 colorAlpha; float4 params; };
StructuredBuffer<Material> g_materials;
Texture2D<float4> g_envMap;
SamplerState      g_envMap_sampler;
RWTexture2D<float4> g_accum;

static const float kPi = 3.14159265359;

float Rand (inout uint s)
{
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return float (w) * (1.0 / 4294967296.0);
}

float2 EnvUv (float3 d)
{
    float theta = acos (clamp (d.z, -1.0, 1.0));
    float phi = atan2 (d.y, d.x) + g_env.y;
    const float twoPi = 6.28318530718;
    phi = phi - twoPi * floor (phi / twoPi);
    return float2 (phi / twoPi, theta / kPi);
}

float3 SkyRadiance (float3 d)
{
    if (g_env.z > 0.5)
        return g_envMap.SampleLevel (g_envMap_sampler, EnvUv (d), 0.0).rgb * g_env.x;
    return g_sky.w * lerp (g_ground.rgb, g_sky.rgb, saturate (d.z * 0.5 + 0.5));
}

bool Clipped (float3 p)
{
    return g_cap.w > 0.5 && dot (g_plane.xyz, p) + g_plane.w < 0.0;
}

struct Hit { bool hit; float t; uint prim; };

Hit Trace (float3 o, float3 d, bool anyHit, inout uint rng)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = 1.0e30;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline (g_scene, anyHit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed ())
    {
        if (q.CandidateType () != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        float t = q.CandidateTriangleRayT ();
        if (Clipped (o + d * t))
            continue;
        float alpha = g_materials[g_triangleMaterial[q.CandidatePrimitiveIndex ()]].colorAlpha.a;
        if (alpha >= 0.99 || Rand (rng) < alpha)
            q.CommitNonOpaqueTriangleHit ();
    }
    Hit h;
    h.hit = q.CommittedStatus () == COMMITTED_TRIANGLE_HIT;
    h.t = h.hit ? q.CommittedRayT () : 0.0;
    h.prim = h.hit ? q.CommittedPrimitiveIndex () : 0;
    return h;
}

float3 CosineSample (float3 n, inout uint rng)
{
    float u = Rand (rng), v = Rand (rng);
    float r = sqrt (u), phi = 2.0 * kPi * v;
    float3 t = abs (n.z) < 0.999 ? normalize (cross (float3 (0, 0, 1), n)) : float3 (1, 0, 0);
    float3 b = cross (n, t);
    return normalize (t * (r * cos (phi)) + b * (r * sin (phi)) + n * sqrt (max (0.0, 1.0 - u)));
}

float Epsilon (float3 p)
{
    float m = max (abs (p.x), max (abs (p.y), abs (p.z)));
    return max (1.0e-3, m * 4.8e-7);
}

float3 Radiance (float3 o, float3 d, inout uint rng)
{
    float3 L = 0.0, T = 1.0;
    for (uint i = 0; i <= g_frame.w; ++i)
    {
        Hit h = Trace (o, d, false, rng);
        if (!h.hit)
        {
            if (i > 0 || (g_env.w > 0.5 && g_env.z > 0.5))
                L += T * SkyRadiance (d);
            break;
        }
        float3 p = o + d * h.t;
        uint i0 = g_indices[h.prim * 3], i1 = g_indices[h.prim * 3 + 1], i2 = g_indices[h.prim * 3 + 2];
        float3 a = g_positions[i0], b = g_positions[i1], c = g_positions[i2];
        float3 n = cross (b - a, c - a);
        n = dot (n, n) > 0.0 ? normalize (n) : -d;
        bool back = dot (n, d) > 0.0;
        if (back)
            n = -n;
        Material m = g_materials[g_triangleMaterial[h.prim]];
        float3 albedo = m.colorAlpha.rgb;
        if (back && g_cap.w > 0.5 && m.colorAlpha.a >= 0.99)
        {
            // The cut: the cap colour, lying in the plane, facing the ray.
            float facing = dot (g_plane.xyz, d);
            if (abs (facing) > 1.0e-6)
            {
                float tPlane = -(dot (g_plane.xyz, o) + g_plane.w) / facing;
                if (tPlane > 0.0 && tPlane < h.t)
                    p = o + d * tPlane;
            }
            n = facing < 0.0 ? g_plane.xyz : -g_plane.xyz;
            albedo = g_cap.rgb;
        }
        float3 at = p + n * Epsilon (p);
        float ndl = dot (n, g_sun.xyz);
        if (ndl > 0.0 && g_sun.w > 0.0)
        {
            Hit s = Trace (at, g_sun.xyz, true, rng);
            if (!s.hit)
                L += T * albedo * (g_sun.w * ndl);
        }
        if (i == g_frame.w)
            break;
        T *= albedo;
        o = at;
        d = CosineSample (n, rng);
    }
    return L;
}

[numthreads (8, 8, 1)]
void main (uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_frame.x || id.y >= g_frame.y)
        return;
    float4 acc = g_frame.z == 0 ? float4 (0, 0, 0, 1) : g_accum[id.xy];
    uint count = max (uint (g_ray.y), 1u);
    for (uint s = 0; s < count; ++s)
    {
        uint sample = g_frame.z + s;
        uint rng = (id.x * 1973u + id.y * 9277u + sample * 26699u) | 1u;
        Rand (rng);
        float2 px = float2 (id.xy) + float2 (Rand (rng), Rand (rng));
        float2 ndc = float2 (px.x / g_frame.x * 2.0 - 1.0, 1.0 - px.y / g_frame.y * 2.0);
        float4 a = mul (g_invViewProj, float4 (ndc, g_ray.x, 1.0));
        float4 b = mul (g_invViewProj, float4 (ndc, 0.5, 1.0));
        float3 o = a.xyz / a.w;
        float3 d = normalize (b.xyz / b.w - o);
        float3 L = Radiance (o, d, rng);
        if (any (isnan (L)) || any (isinf (L)))
            continue;
        acc.rgb += (L - acc.rgb) / float (sample + 1);
    }
    g_accum[id.xy] = float4 (acc.rgb, 1.0);
}
)hlsl";

} // namespace geomsrv::archviz

#endif
