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
// ⚠️ THE SECTION PLANE IS A CANDIDATE TEST. Every triangle is non-opaque so each candidate passes
// through the loop: one beyond the plane is skipped (the clip). A back face hit with the plane on
// is the cut: shaded as the cap colour lying in the plane, the same reading the raster's back-face
// pass gives.
//
// ⚠️ GLASS IS DECIDED BY WHO ASKS (TraceMode). A shadow ray and the Rendered style's one-level
// passes commit a translucent candidate with probability alpha (stochastic transparency, which
// converges to the blend). The path tracer commits every pane and splits it itself, the raster's
// way (kArchVizMeshPS, "THE TRANSMITTED HALF IS SCALED BY ALPHA AND THE REFLECTED HALF IS NOT"):
// the reflection at full Fresnel strength, the diffuse film times alpha, and the rest straight
// through times (1 - alpha). Committed stochastically, a 70 %-transparent pane reflected the sky
// at 30 % strength -- the fault the raster had and fixed (#231 slice 2, RenderBench `glazing`).
//
// Constants mirror RayTracedConstants in DiligentSceneRayTraced.cpp field for field.

namespace geomsrv::archviz {

// The shared half: constants, scene resources, the trace, and the surface at a hit.
inline constexpr char kRayTracedCommon[] = R"hlsl(
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
    float4   g_cone;    // x the camera's spread per pixel, radians (texture mip selection)
};

RaytracingAccelerationStructure g_scene;
StructuredBuffer<float3> g_positions;
StructuredBuffer<float2> g_uvs;
StructuredBuffer<float3> g_normals; // #231: Archicad's per-corner normals, zero where none
StructuredBuffer<uint>   g_indices;
StructuredBuffer<uint>   g_triangleMaterial;
// texture: x the slot in g_albedo (< 0 untextured), yz 1 / the size one repeat covers, w mirror
// bits (1 x, 2 y) -- the raster's g_textureParams, resolved by the same gate (TexturesShown).
struct Material { float4 colorAlpha; float4 params; float4 texture; };
StructuredBuffer<Material> g_materials;
Texture2D<float4> g_envMap;
SamplerState      g_envMap_sampler;
// #231: Archicad's images, sRGB-formatted so a sample is linear. RT_TEXTURE_SLOTS is
// kRayTracedTextureSlots, defined by the C++ side; unused slots hold a white texel.
Texture2D<float4> g_albedo[RT_TEXTURE_SLOTS];
SamplerState      g_albedoSampler;

static const float kPi = 3.14159265359;

float Rand (inout uint s)
{
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return float (w) * (1.0 / 4294967296.0);
}

uint HashU (uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A NESTED HASH, NOT A SUM. The seed was x*1973 + y*9277 + n*26699, and a linear seed repeats:
// pixel (x, y) at sample n started the very sequence of pixel (x-61, y-82) at sample n+33, so the
// two shared every path but 33 of them (74 such pairs inside 960x540x512; RenderBench bathroom:
// the 64-sample error correlated +0.10 at that offset, 0.00 at a neighbouring one).
uint Seed (uint2 px, uint n)
{
    return HashU (px.x + HashU (px.y + HashU (n)));
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

struct Hit { bool hit; float t; uint prim; float2 bary; };

// What a translucent candidate is to the ray (see the header): a shadow ray stops at the first
// committed hit; the stochastic modes commit a pane with probability alpha; kTraceClosest commits
// every surface and leaves the pane to the caller.
static const uint kTraceShadow = 0u;
static const uint kTraceStochastic = 1u;
static const uint kTraceClosest = 2u;

Hit Trace (float3 o, float3 d, uint mode, inout uint rng)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0;
    ray.TMax = 1.0e30;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline (g_scene, mode == kTraceShadow ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, 0xFF,
                      ray);
    while (q.Proceed ())
    {
        if (q.CandidateType () != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        float t = q.CandidateTriangleRayT ();
        if (Clipped (o + d * t))
            continue;
        float alpha = g_materials[g_triangleMaterial[q.CandidatePrimitiveIndex ()]].colorAlpha.a;
        if (mode == kTraceClosest || alpha >= 0.99 || Rand (rng) < alpha)
            q.CommitNonOpaqueTriangleHit ();
    }
    Hit h;
    h.hit = q.CommittedStatus () == COMMITTED_TRIANGLE_HIT;
    h.t = h.hit ? q.CommittedRayT () : 0.0;
    h.prim = h.hit ? q.CommittedPrimitiveIndex () : 0;
    h.bary = h.hit ? q.CommittedTriangleBarycentrics () : float2 (0, 0);
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

)hlsl"
                                           // ⚠️ TWO ADJACENT LITERALS: MSVC caps one string literal near 16 KB (C2026).
                                           R"hlsl(
// THE G-BUFFER PASSES START FROM A POINT REBUILT FROM DEPTH, which is only as exact as the depth:
// tens of metres away the rebuilt point lies centimetres off the surface, and Epsilon's millimetre
// left half the rays starting under it -- every one hit its own surface, so a large model's sky
// light and sun went black in streaks and speckles that changed every frame (Bistro aerial).
// The offset grows with the pixel's footprint at that distance, the scale the depth resolves:
// one footprint cleared the streaks on RenderBench Bistro, 0 left them; 1.5 is the margin.
float GBufferOffset (float3 p, float distance)
{
    return max (Epsilon (p), distance * g_cone.x * 1.5);
}

// The surface a committed hit landed on: position, normal facing the ray, base colour and finish
// (params: x roughness, y metallic, z reflectance -- the raster's DrawnSurface) and alpha. A back
// face with the section on is the cut: the cap colour lying in the plane, facing the ray, matte.
//
// #231: a textured surface takes its texture's colour, as the raster's does (SampleAlbedoMap): uv
// over the repeat size, v growing upward, every other repeat mirrored when asked; the texture
// REPLACES the colour and the opacity stays the surface's. The mip is the ray cone's (Akenine-
// Moller et al., "Improved Shader and Texture Level of Detail Using Ray Cones", 2021): the width
// the pixel's cone has reached at the hit -- `travelled` is the path length before `o` -- over the
// triangle's texel density, widened at grazing angles. A path's bounces keep the camera's cone,
// which over-sharpens a texture seen in a rough reflection; the accumulation averages that out.
float3 TextureColour (Material m, Hit h, uint i0, uint i1, uint i2, float3 a, float3 b, float3 c, float3 d,
                      float travelled)
{
    float2 u0 = g_uvs[i0], u1 = g_uvs[i1], u2 = g_uvs[i2];
    float2 uv = u0 + h.bary.x * (u1 - u0) + h.bary.y * (u2 - u0);
    float2 t = uv * m.texture.yz;
    float2 s = float2 (t.x, -t.y);
    uint mirror = (uint) (m.texture.w + 0.5);
    if ((mirror & 1u) != 0u)
        s.x = 1.0 - abs (frac (s.x * 0.5) * 2.0 - 1.0);
    if ((mirror & 2u) != 0u)
        s.y = 1.0 - abs (frac (s.y * 0.5) * 2.0 - 1.0);
    uint slot = (uint) (m.texture.x + 0.5);
    uint width, height, levels;
    g_albedo[NonUniformResourceIndex (slot)].GetDimensions (0, width, height, levels);
    float2 texels = m.texture.yz * float2 (width, height);
    float2 e1 = (u1 - u0) * texels, e2 = (u2 - u0) * texels;
    float texelArea = abs (e1.x * e2.y - e1.y * e2.x);
    float3 nw = cross (b - a, c - a);
    float worldArea = length (nw);
    float cosine = max (abs (dot (nw / max (worldArea, 1.0e-20), d)), 0.05);
    float cone = g_cone.x * (travelled + h.t);
    float lod = 0.5 * log2 (max (texelArea / max (worldArea, 1.0e-20), 1.0e-20)) + log2 (max (cone, 1.0e-9)) -
                log2 (cosine);
    return g_albedo[NonUniformResourceIndex (slot)].SampleLevel (g_albedoSampler, s, max (lod, 0.0)).rgb;
}

//
// #231: TWO NORMALS. `n` shades: Archicad's per-corner normals interpolated across the triangle,
// as the raster interpolates them, so a tessellated column or sphere reads as curved instead of
// as facets (RenderBench cornell-sphere: a mirror sphere reflected in blocks, its sun highlight
// one square facet). `ng` is the flat triangle's, facing the ray, and decides geometry: which side
// a ray leaves from and whether a new direction goes into the surface. The welder already splits
// the normals at creases, so a box's faces stay flat. An element without normals (zeros) shades flat.
void SurfaceFull (Hit h, float3 o, float3 d, float travelled, out float3 p, out float3 n, out float3 ng,
                  out float3 albedo, out float3 finish, out float alpha)
{
    p = o + d * h.t;
    uint i0 = g_indices[h.prim * 3], i1 = g_indices[h.prim * 3 + 1], i2 = g_indices[h.prim * 3 + 2];
    float3 a = g_positions[i0], b = g_positions[i1], c = g_positions[i2];
    ng = cross (b - a, c - a);
    ng = dot (ng, ng) > 0.0 ? normalize (ng) : -d;
    bool back = dot (ng, d) > 0.0;
    if (back)
        ng = -ng;
    float3 ns = g_normals[i0] + h.bary.x * (g_normals[i1] - g_normals[i0]) + h.bary.y * (g_normals[i2] - g_normals[i0]);
    n = dot (ns, ns) > 1.0e-12 ? normalize (ns) : ng;
    if (dot (n, ng) < 0.0)
        n = -n;
    Material m = g_materials[g_triangleMaterial[h.prim]];
    albedo = m.texture.x >= 0.0 ? TextureColour (m, h, i0, i1, i2, a, b, c, d, travelled) : m.colorAlpha.rgb;
    finish = m.params.xyz;
    alpha = m.colorAlpha.a;
    if (back && g_cap.w > 0.5 && m.colorAlpha.a >= 0.99)
    {
        finish = float3 (1.0, 0.0, 0.5);
        float facing = dot (g_plane.xyz, d);
        if (abs (facing) > 1.0e-6)
        {
            float tPlane = -(dot (g_plane.xyz, o) + g_plane.w) / facing;
            if (tPlane > 0.0 && tPlane < h.t)
                p = o + d * tPlane;
        }
        n = facing < 0.0 ? g_plane.xyz : -g_plane.xyz;
        ng = n;
        albedo = g_cap.rgb;
    }
}

// The sun's irradiance at an offset point, shadow ray included.
float3 SunLight (float3 at, float3 n, inout uint rng)
{
    float ndl = dot (n, g_sun.xyz);
    if (ndl <= 0.0 || g_sun.w <= 0.0)
        return 0.0;
    Hit s = Trace (at, g_sun.xyz, kTraceShadow, rng);
    return s.hit ? 0.0 : (g_sun.w * ndl).xxx;
}

float Luminance (float3 c)
{
    return dot (c, float3 (0.2126, 0.7152, 0.0722));
}

float3 FresnelSchlick (float3 f0, float cosTheta)
{
    float k = pow (1.0 - saturate (cosTheta), 5.0);
    return f0 + (1.0 - f0) * k;
}

// Smith-GGX masking for one direction, alpha = roughness^2 (the raster's convention).
float SmithG1 (float ndx, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0 * ndx / (ndx + sqrt (a2 + (1.0 - a2) * ndx * ndx));
}

// A GGX half vector around n, distributed by D (so the weight below needs no D).
float3 SampleGgxHalf (float3 n, float alpha, inout uint rng)
{
    float u = Rand (rng), w = Rand (rng);
    float phi = 2.0 * kPi * u;
    float cosTheta = sqrt ((1.0 - w) / (1.0 + (alpha * alpha - 1.0) * w));
    float sinTheta = sqrt (max (0.0, 1.0 - cosTheta * cosTheta));
    float3 t = abs (n.z) < 0.999 ? normalize (cross (float3 (0, 0, 1), n)) : float3 (1, 0, 0);
    float3 b = cross (n, t);
    return normalize (t * (sinTheta * cos (phi)) + b * (sinTheta * sin (phi)) + n * cosTheta);
}

// #232: the specular lobe the raster draws, so the reference can judge reflections. The sun is
// a delta light, so its highlight is the GGX BRDF evaluated once; roughness is floored for it
// alone, as the raster's analytic highlight is, or a mirror would carry one infinite pixel.
float3 SunSpecular (float3 n, float3 v, float3 f0, float rough, float3 sunIrradiance)
{
    float3 l = g_sun.xyz;
    float ndl = dot (n, l);
    float ndv = max (dot (n, v), 1.0e-4);
    if (ndl <= 0.0 || all (sunIrradiance <= 0.0))
        return 0.0;
    float3 hv = normalize (l + v);
    float ndh = saturate (dot (n, hv));
    float alpha = max (rough * rough, 0.05 * 0.05);
    float a2 = alpha * alpha;
    float den = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (kPi * den * den);
    float G = SmithG1 (ndv, alpha) * SmithG1 (ndl, alpha);
    float3 F = FresnelSchlick (f0, saturate (dot (v, hv)));
    // sunIrradiance carries the raster's Lambert convention (albedo * E is radiance), so the
    // physical irradiance is pi * E -- the same factor the diffuse lobe hides.
    return D * G * F / (4.0 * ndl * ndv) * kPi * sunIrradiance;
}

// #232: an indirect contribution brighter than four sunlit white walls is a firefly -- a rare
// diffuse-then-glossy path that would take thousands of samples to average away. Capped, the
// reference converges in hundreds; the bias touches only those paths.
float3 Indirect (float3 c, uint bounce)
{
    if (bounce == 0)
        return c;
    float cap = 4.0 * max (g_sun.w, 1.0);
    float y = Luminance (c);
    return y > cap ? c * (cap / y) : c;
}

// Panes a path may pass straight through on top of its bounces: a pass costs no bounce (seeing
// through a window is not interreflection), so this is what stops a stack of glass looping.
static const uint kMaxPanes = 8u;

float3 Radiance (float3 o, float3 d, inout uint rng)
{
    float3 L = 0.0, T = 1.0;
    uint i = 0;             // bounces so far
    float travelled = 0.0;  // path length to o, for the texture cone
    for (uint step = 0; step <= g_frame.w + kMaxPanes; ++step)
    {
        Hit h = Trace (o, d, kTraceClosest, rng);
        if (!h.hit)
        {
            if (i > 0 || (g_env.w > 0.5 && g_env.z > 0.5))
                L += Indirect (T * SkyRadiance (d), i);
            break;
        }
        float3 p, n, ng, albedo, finish;
        float alpha;
        SurfaceFull (h, o, d, travelled, p, n, ng, albedo, finish, alpha);
        travelled += h.t;
        // The pane's split (see the header): its diffuse film is alpha of it, the rest passes.
        float film = alpha >= 0.99 ? 1.0 : saturate (alpha);
        float rough = saturate (finish.x), metal = saturate (finish.y);
        float3 f0 = lerp ((0.08 * saturate (finish.z)).xxx, albedo, metal);
        float3 diffuse = albedo * (1.0 - metal);
        float3 v = -d;
        float ndv = max (dot (n, v), 1.0e-4);
        float3 F = FresnelSchlick (f0, ndv);
        float3 at = p + ng * Epsilon (p);
        float3 sun = SunLight (at, n, rng);
        L += Indirect (T * (film * diffuse * (1.0 - F) * sun + SunSpecular (n, v, f0, rough, sun)), i);
        float passShare = 1.0 - film;
        if (i == g_frame.w)
        {
            // Out of bounces: no lobe may scatter, but a pane is still seen through.
            if (passShare <= 0.0)
                break;
            T *= passShare;
            o = p + d * Epsilon (p);
            continue;
        }
        // One lobe per event, chosen by its share of the energy: reflect, diffuse, or pass.
        float specularShare = Luminance (F);
        float diffuseShare = film * Luminance (diffuse * (1.0 - F));
        float total = max (specularShare + diffuseShare + passShare, 1.0e-4);
        float pSpecular = clamp (specularShare / total, 0.05, 1.0);
        float pPass = (1.0 - pSpecular) * passShare / max (passShare + diffuseShare, 1.0e-4);
        float u = Rand (rng);
        if (u >= pSpecular && u < pSpecular + pPass)
        {
            T *= passShare / pPass;
            o = p + d * Epsilon (p);
            continue;
        }
        ++i;
        o = at;
        if (u < pSpecular)
        {
            float alpha = max (rough * rough, 1.0e-4);
            float3 hv = SampleGgxHalf (n, alpha, rng);
            float3 l = reflect (d, hv);
            float ndl = dot (n, l);
            // Below the triangle itself: the smooth normal bent the lobe into the surface.
            if (ndl <= 0.0 || dot (l, ng) <= 0.0)
                break;
            float vdh = saturate (dot (v, hv));
            float ndh = max (dot (n, hv), 1.0e-4);
            // D-sampled GGX: f * cos / pdf = F * G * vdh / (ndh * ndv).
            T *= FresnelSchlick (f0, vdh) * SmithG1 (ndv, alpha) * SmithG1 (ndl, alpha) * vdh / (ndh * ndv) /
                 pSpecular;
            d = l;
        }
        else
        {
            T *= film * diffuse * (1.0 - F) / max (1.0 - pSpecular - pPass, 1.0e-4);
            d = CosineSample (n, rng);
            if (dot (d, ng) <= 0.0)
                break;
        }
    }
    return L;
}

)hlsl";

// The style: a running mean of full paths per pixel.
inline constexpr char kRayTracedAccumulateMain[] = R"hlsl(
RWTexture2D<float4> g_accum;

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
        uint rng = Seed (id.xy, sample);
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

// Traced reflections for the Rendered style (#232): one pass over the G-buffer writing what
// DiligentFX's SSR writes -- reflected radiance and a confidence -- so the SSR composite is
// unchanged. Every glossy pixel reflects the real model, offscreen and occluded parts included.
// The hit is lit by the sun (with its shadow ray) and the sky's diffuse light; no second bounce.
// g_ray.z is the environment's last mip, g_ray.w the roughness threshold, g_frame.z the frame.
inline constexpr char kRayTracedReflectMain[] = R"hlsl(
Texture2D<float>  g_depth;
Texture2D<float4> g_normal;
Texture2D<float>  g_roughness;
RWTexture2D<float4> g_reflection;

float3 SkyLevel (float3 d, float level)
{
    if (g_env.z > 0.5)
        return g_envMap.SampleLevel (g_envMap_sampler, EnvUv (d), level).rgb * g_env.x;
    return g_sky.w * lerp (g_ground.rgb, g_sky.rgb, saturate (d.z * 0.5 + 0.5));
}

// GGX half-vector sample around n (perceptual roughness), reflected about it.
float3 SampleReflection (float3 v, float3 n, float rough, inout uint rng)
{
    float a = max (rough * rough, 1.0e-4);
    float u = Rand (rng), w = Rand (rng);
    float phi = 2.0 * kPi * u;
    float cosTheta = sqrt ((1.0 - w) / (1.0 + (a * a - 1.0) * w));
    float sinTheta = sqrt (max (0.0, 1.0 - cosTheta * cosTheta));
    float3 t = abs (n.z) < 0.999 ? normalize (cross (float3 (0, 0, 1), n)) : float3 (1, 0, 0);
    float3 b = cross (n, t);
    float3 h = normalize (t * (sinTheta * cos (phi)) + b * (sinTheta * sin (phi)) + n * cosTheta);
    return reflect (v, h);
}

[numthreads (8, 8, 1)]
void main (uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_frame.x || id.y >= g_frame.y)
        return;
    float depth = g_depth.Load (int3 (id.xy, 0));
    float rough = g_roughness.Load (int3 (id.xy, 0));
    bool background = g_ray.x < 0.5 ? depth >= 0.9999999 : depth <= 1.0e-7;
    if (background || rough > g_ray.w)
    {
        g_reflection[id.xy] = float4 (0, 0, 0, 0);
        return;
    }
    float2 ndc = float2 ((id.x + 0.5) / g_frame.x * 2.0 - 1.0, 1.0 - (id.y + 0.5) / g_frame.y * 2.0);
    float4 a = mul (g_invViewProj, float4 (ndc, g_ray.x, 1.0));
    float4 w = mul (g_invViewProj, float4 (ndc, depth, 1.0));
    float3 eye = a.xyz / a.w;
    float3 p = w.xyz / w.w;
    float3 v = normalize (p - eye);
    float3 n = g_normal.Load (int3 (id.xy, 0)).xyz;
    n = dot (n, n) > 0.0 ? normalize (n) : -v;
    if (dot (n, v) > 0.0)
        n = -n;
    float3 at = p + n * GBufferOffset (p, length (p - eye));

    uint rng = Seed (id.xy, g_frame.z);
    // ONE RAY FOR A MIRROR, FOUR FOR A GLOSS: the GGX lobe is sampled, and TAA (on in Rendered)
    // averages the remaining noise across frames.
    uint count = rough < 0.03 ? 1u : 4u;
    float3 sum = 0.0;
    for (uint s = 0; s < count; ++s)
    {
        float3 d = count == 1u ? reflect (v, n) : SampleReflection (v, n, rough, rng);
        if (dot (d, n) <= 0.0)
            d = reflect (v, n);
        Hit h = Trace (at, d, kTraceStochastic, rng);
        if (!h.hit)
        {
            sum += SkyLevel (d, 0.0);
            continue;
        }
        float3 hp, hn, hng, albedo, finish;
        float alpha;
        SurfaceFull (h, at, d, length (p - eye), hp, hn, hng, albedo, finish, alpha);
        float3 hat = hp + hng * Epsilon (hp);
        // #232: THE SKY AT THE HIT IS SEEN THROUGH ONE VISIBILITY RAY, not assumed. Unoccluded,
        // every wall a reflection landed on inside a closed room was lit as if it stood outdoors,
        // and an indoor mirror glowed (RenderBench bathroom: the mirror twice the reference's
        // brightness). One cosine-distributed ray per sample; TAA averages the rest.
        float3 skyDir = CosineSample (hn, rng);
        float skySeen = Trace (hat, skyDir, kTraceShadow, rng).hit ? 0.0 : 1.0;
        float metal = saturate (finish.y);
        float3 diffuseLight = SunLight (hat, hn, rng) + SkyLevel (hn, g_ray.z) * skySeen;
        // A metal seen in a reflection has no diffuse lobe; it shows its own reflection of the sky
        // (unoccluded -- one level deep is where this pass stops).
        float3 mirrored = SkyLevel (reflect (d, hn), g_ray.z * saturate (finish.x));
        sum += albedo * lerp (diffuseLight, mirrored, metal);
    }
    g_reflection[id.xy] = float4 (sum / float (count), 1.0);
}
)hlsl";

// #233: traced sky light for the Rendered style. The raster's sky ambient is occluded only by
// GTAO, whose reach is a metre or two, so a closed room was lit as if it stood outdoors. Here a
// few cosine rays per pixel ask what the point really sees: the sky where a ray escapes, and the
// sunlit diffuse surface where it lands (one bounce, sun only). The ratio of that to the open sky
// along the same rays is the share of the raster's ambient the point keeps -- capped at 1, so an
// outdoor point is never brightened and keeps GTAO's contact detail -- written in the AO
// texture's place, with the AO intensity already applied (g_ray.w). TAA averages the noise.
//
// y is the sun's visibility from one shadow ray, which the raster takes in place of its shadow
// map on opaque surfaces: a shadow map's depth bias leaks light along every zero-thickness join
// (RenderBench: a bright line where each Cornell wall meets the next). -1 where there is no
// G-buffer surface, so whatever the raster draws there keeps the shadow map.
inline constexpr char kRayTracedSkyMain[] = R"hlsl(
Texture2D<float>  g_depth;
Texture2D<float4> g_normal;
Texture2D<float>  g_occlusion;
RWTexture2D<float2> g_skyLight;

float3 SkyDiffuse (float3 d)
{
    if (g_env.z > 0.5)
        return g_envMap.SampleLevel (g_envMap_sampler, EnvUv (d), g_ray.z).rgb * g_env.x;
    return g_sky.w * lerp (g_ground.rgb, g_sky.rgb, saturate (d.z * 0.5 + 0.5));
}

[numthreads (8, 8, 1)]
void main (uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_frame.x || id.y >= g_frame.y)
        return;
    float depth = g_depth.Load (int3 (id.xy, 0));
    float gtao = lerp (1.0, saturate (g_occlusion.Load (int3 (id.xy, 0))), saturate (g_ray.w));
    bool background = g_ray.x < 0.5 ? depth >= 0.9999999 : depth <= 1.0e-7;
    if (background)
    {
        g_skyLight[id.xy] = float2 (gtao, -1.0);
        return;
    }
    float2 ndc = float2 ((id.x + 0.5) / g_frame.x * 2.0 - 1.0, 1.0 - (id.y + 0.5) / g_frame.y * 2.0);
    float4 a = mul (g_invViewProj, float4 (ndc, g_ray.x, 1.0));
    float4 w = mul (g_invViewProj, float4 (ndc, depth, 1.0));
    float3 eye = a.xyz / a.w;
    float3 p = w.xyz / w.w;
    float3 v = normalize (p - eye);
    float3 n = g_normal.Load (int3 (id.xy, 0)).xyz;
    n = dot (n, n) > 0.0 ? normalize (n) : -v;
    if (dot (n, v) > 0.0)
        n = -n;
    float3 at = p + n * GBufferOffset (p, length (p - eye));

    uint rng = Seed (id.xy, g_frame.z);
    float open = 0.0, seen = 0.0;
    for (uint s = 0; s < 4u; ++s)
    {
        float3 d = CosineSample (n, rng);
        float sky = Luminance (SkyDiffuse (d));
        open += sky;
        Hit h = Trace (at, d, kTraceStochastic, rng);
        if (!h.hit)
        {
            seen += sky;
            continue;
        }
        float3 hp, hn, hng, albedo, finish;
        float alpha;
        SurfaceFull (h, at, d, length (p - eye), hp, hn, hng, albedo, finish, alpha);
        float3 hat = hp + hng * Epsilon (hp);
        seen += Luminance (albedo * (1.0 - saturate (finish.y)) * SunLight (hat, hn, rng));
    }
    float traced = saturate (seen / max (open, 1.0e-4));
    // g_ray.y: the raster's shadows are on. Off, the sun stays unshadowed, as the raster draws it.
    float sun = g_ray.y > 0.5 ? 1.0 : -1.0;
    if (g_ray.y > 0.5 && g_sun.w > 0.0)
        sun = dot (n, g_sun.xyz) <= 0.0 || Trace (at, g_sun.xyz, kTraceShadow, rng).hit ? 0.0 : 1.0;
    g_skyLight[id.xy] = float2 (min (gtao, traced), sun);
}
)hlsl";

} // namespace geomsrv::archviz

#endif
