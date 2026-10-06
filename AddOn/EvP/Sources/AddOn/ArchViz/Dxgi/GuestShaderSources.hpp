#ifndef EVP_ARCHVIZ_DXGI_GUESTSHADERSOURCES_HPP
#define EVP_ARCHVIZ_DXGI_GUESTSHADERSOURCES_HPP

// ArchViz/Dxgi/GuestShaderSources -- the HLSL the Diligent guest draws the overlays'
// rich content with: fills (styled meshes, heatmaps, legend bars), pixel-wide lines
// (dashed, depth-styled) and MTSDF glyph quads (text, dimension furniture). One
// header so tests/cpp/test_overlayshaders.cpp compiles every entry point and reads
// each vertex shader's inputs back against ArchViz/OverlayScene.hpp's layouts.
//
// ⚠️ THE PIXEL SHADERS ARE SHARED; THE PROJECTIONS ARE NOT (§12). `kCommon` is what
// both overlays paint with. `kPlanBody` projects with the walls' own arithmetic --
// the transform ACAPI read at the plan's Present, hi/lo halves under `precise`
// (PlanOverlayContent.hpp) -- and has no camera. `kSceneBody` projects with
// `ArchicadClip`, which `camerashader::Compose` writes per camera slot, and reads no
// plan transform. Neither body knows the other exists.
//
// ⚠️ DILIGENT'S VERTEX SEMANTICS ARE `ATTRIBn`, n BEING THE LAYOUT ELEMENT'S INDEX.
// The guest's input layouts (Dxgi/GuestGpu.cpp) number the fields of the
// OverlayScene structs in order; a shader reading ATTRIB5 as something else draws
// confidently wrong, which is why the test reflects on them.
//
// ⚠️ ARCHICAD'S BACK BUFFER IS UNORM, NOT sRGB. Colours arrive as the bytes a caller
// wrote and leave as those bytes: nothing here linearises, unlike the viewer's text
// layer, whose target is an sRGB view. The atlas is linear DATA, sampled as such.
//
// ⚠️ THE PASS DECIDES DEPTH, THE ITEM DECIDES WHETHER IT IS IN THE PASS. 3D draws
// in up to three passes: 0 "near", tested LESS_EQUAL, the part in front of the
// building; 1 "behind", tested GREATER, the hidden part, faint or dashed; 2 "over",
// untested. `InPass` is the table. The plan has one pass, 2, and no depth
// (finding 13).

#include <string>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace guestshaders {

inline constexpr const char* kCommon = R"hlsl(
cbuffer GuestDraw
{
    float4 Mode;          // x shading (0 flat, 1 lit, 2 ghost, 3 x-ray), y opacity,
                          // z pass (0 near, 1 behind, 2 over, 3 all), w 1 for a fill fixed to the view
    float4 Ramp;          // x min, y 1 / (max - min), z bands (0 smooth), w stops (0: no ramp)
    float4 Iso;           // x step (0: none), y half width in physical pixels
    float4 IsoColour;
    float4 StopAt[4];     // 16 positions, 0..1
    float4 StopColour[16];
    float4 Atlas;         // x 1 / page width, y 1 / page height, z distance range and w the em in atlas pixels
    float4 Dashes[32];    // 16 dash patterns of 8 lengths in metres: on, off, on, off...
    float4 Highlight;     // x low, y high, z 1: a heatmap whose legend is pointed at
    float4 Hatch;         // xy model-XY stripe normal, z lines per metre, w enabled
    float4 SurfaceStyle;  // x retained alpha behind buildings
};

struct FillOut
{
    float4 position : SV_POSITION;
    float4 colour : COLOR0;
    float value : TEXCOORD0;
    float3 normalView : TEXCOORD1;
    float2 hatchXY : TEXCOORD2;
};

struct LineOut
{
    float4 position : SV_POSITION;
    float4 colour : COLOR0;
    float across : TEXCOORD0;     // signed physical pixels from the centre line
    // Metres along the polyline over w, and 1 over w: interpolated linearly on the screen
    // (every line vertex is at w 1), their ratio is the fragment's metres, perspective-correct.
    float2 metres : TEXCOORD1;
    float halfWidth : TEXCOORD2;  // physical pixels
    nointerpolation uint dash : TEXCOORD3; // the pattern in Dashes, or 255: solid
};

struct GlyphOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
    float4 halo : COLOR1;
    float haloPixels : TEXCOORD1;
    float solid : TEXCOORD2;
};

static const float4 kCulled = float4 (2.0, 2.0, 2.0, 1.0); // outside every clip plane

float StopPosition (int k)
{
    return StopAt[k >> 2][k & 3];
}

// The same piecewise-linear ramp as overlayscene::RampAt.
float4 RampColour (float t)
{
    int count = (int) Ramp.w;
    float4 c = StopColour[0];
    [loop] for (int k = 1; k < count; ++k)
    {
        float a0 = StopPosition (k - 1);
        float a1 = StopPosition (k);
        if (t >= a0)
            c = lerp (StopColour[k - 1], StopColour[k], a1 > a0 ? saturate ((t - a0) / (a1 - a0)) : 1.0);
    }
    return c;
}

bool InPass (uint behind)
{
    if (Mode.z > 2.5)
        return true; // 3, "all": no depth to split by, so everything is drawn whole
    if (Mode.z < 0.5)
        return behind != 3u;
    if (Mode.z < 1.5)
        return behind == 1u || behind == 2u;
    return behind == 3u;
}

// A heatmap's fragment outside the band its legend is pointed at (OverlayHud.hpp):
// mostly grey and faint, so the band stands out. Unchanged with no band.
float4 Highlighted (float4 c, float value)
{
    if (Highlight.z < 0.5 || (value >= Highlight.x && value <= Highlight.y))
        return c;
    float grey = dot (c.rgb, float3 (0.299, 0.587, 0.114));
    return float4 (lerp (c.rgb, float3 (grey, grey, grey), 0.75), c.a * 0.35);
}

float HatchCoverage (float stripes, float derivative)
{
    float d = abs (frac (stripes + 0.5) - 0.5) / max (derivative, 1e-6);
    return derivative > 0.5 ? 0.25 : saturate (1.0 - d);
}

float SurfaceAlpha (float alpha, float renderPass, float retained)
{
    return alpha * ((renderPass > 0.5 && renderPass < 1.5) ? retained : 1.0);
}

float4 PSFill (FillOut i) : SV_TARGET
{
    float4 c = i.colour;
    if (Ramp.w > 0.5)
    {
        float t = saturate ((i.value - Ramp.x) * Ramp.y);
        if (Ramp.z > 0.5)
        {
            float k = min (floor (t * Ramp.z), Ramp.z - 1.0);
            t = Ramp.z > 1.5 ? k / (Ramp.z - 1.0) : 0.5;
        }
        c = RampColour (t);
        // A contour every `step` value units, a fixed number of pixels wide: the
        // value's own screen derivative turns the distance to it into pixels.
        float f = i.value / max (Iso.x, 1e-9);
        float d = abs (frac (f + 0.5) - 0.5) / max (fwidth (f), 1e-6);
        float contour = Iso.x > 0.0 ? saturate (Iso.y - d + 0.5) : 0.0;
        c.rgb = lerp (c.rgb, IsoColour.rgb, contour * IsoColour.a);
        c.a = max (c.a, contour * IsoColour.a);
        c = Highlighted (c, i.value);
    }
    float3 n = i.normalView;
    float facing = dot (n, n) > 1e-12 ? saturate (abs (normalize (n).z)) : 1.0;
    if (Mode.x > 0.5 && Mode.x < 1.5)
        c.rgb *= 0.45 + 0.55 * facing;
    else if (Mode.x > 1.5 && Mode.x < 2.5)
    {
        c.rgb *= 0.75 + 0.25 * facing;
        c.a *= 0.25 + 0.75 * pow (1.0 - facing, 2.5);
    }
    else if (Mode.x > 2.5)
        c.a *= 0.3 + 0.5 * pow (1.0 - facing, 2.0);
    c.a *= Mode.y;
    if (Hatch.w > 0.5) {
        float f = dot (i.hatchXY, Hatch.xy) * Hatch.z + SurfaceStyle.y;
        float coverage = HatchCoverage (f, fwidth (f));
        c.a *= lerp (0.15, 1.0, coverage);
    }
    c.a = SurfaceAlpha (c.a, Mode.z, SurfaceStyle.x);
    return c;
}

// How much of a fragment `metres` along a line pattern `pattern` covers: 1 inside a dash,
// 0 inside a gap, half at an edge, the edges a pixel soft by the metres a pixel spans.
// ⚠️ UNDER A FEW PIXELS A PERIOD, THE DASHES' SHARE OF IT: a pattern seen from far off is
// finer than the pixels and would shimmer as the camera moves, so it fades into an even
// line as faint as the dashes are on average.
float DashCoverage (uint pattern, float metres, float metresPerPixel)
{
    if (pattern > 15u)
        return 1.0;
    float4 first = Dashes[pattern * 2u];
    float4 second = Dashes[pattern * 2u + 1u];
    float lengths[8] = { first.x, first.y, first.z, first.w, second.x, second.y, second.z, second.w };
    float total = 0.0, on = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        total += lengths[k];
        on += (k & 1) == 0 ? lengths[k] : 0.0;
    }
    if (total <= 0.0)
        return 1.0;
    float pixel = max (metresPerPixel, 1e-9);
    float t = metres - total * floor (metres / total);
    float start = 0.0, coverage = 0.0;
    [unroll] for (int j = 0; j < 8; ++j)
    {
        float end = start + lengths[j];
        if (t >= start && t < end)
        {
            float inside = saturate (min (t - start, end - t) / pixel + 0.5);
            coverage = (j & 1) == 0 ? inside : 1.0 - inside;
        }
        start = end;
    }
    return lerp (on / total, coverage, saturate ((total / pixel - 2.0) / 4.0));
}

// Coverage across the width and along the dash pattern -- the antialiasing StorySliceLayer
// established: the ribbon is a pixel wider than the line, and its last pixel fades.
float4 PSLine (LineOut i) : SV_TARGET
{
    // Before the branch: the derivative wants every pixel of the quad.
    float metres = i.metres.x / max (i.metres.y, 1e-12);
    float perPixel = fwidth (metres);
    float coverage = saturate (i.halfWidth - abs (i.across) + 0.5);
    if (i.dash <= 15u)
        coverage *= DashCoverage (i.dash, metres, perPixel);
    if (coverage <= 0.001)
        discard;
    return float4 (i.colour.rgb, i.colour.a * coverage * Mode.y);
}

Texture2D g_atlas;
SamplerState g_atlas_sampler;

float Median (float3 v)
{
    return max (min (v.r, v.g), min (max (v.r, v.g), v.b));
}

// SceneTextLayer's MTSDF resolve: the median of RGB is the edge, alpha the true
// distance for the halo, the uv derivatives turn the atlas range into pixels.
// Premultiplied out. The sample comes before the branch: derivatives stay uniform.
//
// ⚠️ THE HALO STOPS WHERE THE ATLAS STOPS KNOWING. The atlas records distance only
// half its range beyond an edge -- `0.5 * screenRange` screen pixels -- and past that
// every texel of the quad reads as inside a halo that reaches further: a box per
// glyph (the first live run). So the halo is clamped to what the atlas can say.
//
// `solid` 0 is an MTSDF glyph, 1 a flat panel or tick, 2 a plain texture times the
// vertex colour: an ImGui panel's triangles over its own font atlas (OverlayHud.hpp).
//
// ⚠️ AN AUTOMATIC HALO FOLLOWS THE TEXT AS IT IS DRAWN, NOT AS IT WAS ASKED FOR. A fixed
// 1.5 px halo is a dark blot round a label seen far off in the model or set small on
// the view (the live run of 2026-09-29, 10:39). A negative `haloPixels` asks for
// (em - 8) / 32 screen pixels -- 0.5 round a 24 px em, 2 round a 72 px one, none under
// 8 -- times its magnitude; the em on screen is the atlas em times the same screen
// pixels per atlas pixel `screenRange` is made of, so a text on a plane shrinks its
// halo as it recedes. A positive one is fixed. Either way it stops where the atlas does.
float HaloReach (float haloPixels, float screenRange)
{
    float emPixels = screenRange * Atlas.w / Atlas.z;
    float wanted = haloPixels >= 0.0 ? haloPixels : -haloPixels * max ((emPixels - 8.0) / 32.0, 0.0);
    return min (wanted, max (0.5 * screenRange - 0.75, 0.0));
}

float4 PSGlyph (GlyphOut i) : SV_TARGET
{
    float4 d = g_atlas.Sample (g_atlas_sampler, i.uv);
    float2 unitRange = Atlas.z * Atlas.xy;
    float2 screenTexelRange = 1.0 / max (fwidth (i.uv), float2 (1e-6, 1e-6));
    float screenRange = max (0.5 * dot (unitRange, screenTexelRange), 1.0);
    float fade = Mode.y * ((Mode.z > 0.5 && Mode.z < 1.5) ? 0.3 : 1.0);
    if (i.solid > 1.5)
    {
        float4 t = i.colour * d;
        float a = t.a * fade;
        return float4 (t.rgb * a, a);
    }
    if (i.solid > 0.5)
    {
        float a = i.colour.a * fade;
        return float4 (i.colour.rgb * a, a);
    }
    float fill = saturate (screenRange * (Median (d.rgb) - 0.5) + 0.5);
    float reach = HaloReach (i.haloPixels, screenRange);
    float halo = saturate (screenRange * (d.a - 0.5) + 0.5 + reach);
    float fillAlpha = fill * i.colour.a;
    float haloAlpha = halo * i.halo.a * (1.0 - fillAlpha);
    return float4 (i.colour.rgb * fillAlpha + i.halo.rgb * haloAlpha, fillAlpha + haloAlpha) * fade;
}
)hlsl";

// The floor plan: the walls' projection, pixels from model metres, no depth.
inline constexpr const char* kPlanBody = R"hlsl(
cbuffer GuestPlanFrame
{
    float4 Linear;   // physical pixels per metre: xx, xy, yx, yy
    float4 View;     // the anchor relative to the content origin: hi x, hi y, lo x, lo y
    float4 Screen;   // the anchor's physical pixel, then 2 / width, 2 / height
    float4 Surface;  // width, height in physical pixels, the view's DPI scale
};

float2 Pixel (float2 hi, float2 lo)
{
    precise float2 d = (hi - View.xy) + (lo - View.zw);
    return float2 (Linear.x * d.x + Linear.y * d.y, Linear.z * d.x + Linear.w * d.y) + Screen.xy;
}

float2 Turned (float2 dir)
{
    return float2 (Linear.x * dir.x + Linear.y * dir.y, Linear.z * dir.x + Linear.w * dir.y);
}

float4 ClipOf (float2 p)
{
    return float4 (p.x * Screen.z - 1.0, 1.0 - p.y * Screen.w, 0.0, 1.0);
}

FillOut VSFill (float2 hi : ATTRIB0, float2 lo : ATTRIB1, float2 offset : ATTRIB2, float4 colour : ATTRIB3,
                float value : ATTRIB4)
{
    FillOut o;
    float2 p = (Mode.w > 0.5 ? hi * Surface.xy : Pixel (hi, lo)) + offset * Surface.z;
    o.position = ClipOf (p);
    o.colour = colour;
    o.value = value;
    o.normalView = float3 (0.0, 0.0, 1.0);
    o.hatchXY = hi + lo;
    return o;
}

LineOut VSLine (float2 hiA : ATTRIB0, float2 loA : ATTRIB1, float2 hiB : ATTRIB2, float2 loB : ATTRIB3,
                float4 colour : ATTRIB4, float2 style : ATTRIB5, uint dashes : ATTRIB6, uint corner : SV_VertexID)
{
    LineOut o;
    float2 a = Pixel (hiA, loA);
    float2 b = Pixel (hiB, loB);
    float halfWidth = style.x * 0.5 * Surface.z;
    float pushed = halfWidth + 1.0;
    float2 d = b - a;
    float len = length (d);
    float2 along = len > 1e-3 ? d / len : float2 (1.0, 0.0);
    float2 across = float2 (-along.y, along.x);
    // corners 0..5: (a,-) (b,-) (b,+) | (a,-) (b,+) (a,+), square caps
    bool atB = corner == 1 || corner == 2 || corner == 4;
    float side = (corner == 2 || corner == 4 || corner == 5) ? 1.0 : -1.0;
    o.position = ClipOf ((atB ? b + along * pushed : a - along * pushed) + across * (side * pushed));
    o.colour = colour;
    o.across = side * pushed;
    // Metres along the polyline: the plan is flat, so they run evenly across the screen.
    precise float2 m = (hiB - hiA) + (loB - loA);
    float metres = length (m);
    float s = len > 1e-3 ? (atB ? len + pushed : -pushed) / len : (atB ? 1.0 : 0.0);
    o.metres = float2 (style.y + s * metres, 1.0);
    o.halfWidth = halfWidth;
    o.dash = dashes & 255u;
    return o;
}

GlyphOut VSGlyph (float2 hi : ATTRIB0, float2 lo : ATTRIB1, float2 offset : ATTRIB2, float2 uv : ATTRIB3,
                  float2 dir : ATTRIB4, float4 colour : ATTRIB5, float4 halo : ATTRIB6, float haloPixels : ATTRIB7,
                  uint flags : ATTRIB8, float minSpan : ATTRIB9)
{
    GlyphOut o;
    float2 anchor = (flags & 1u) != 0u ? hi * Surface.xy : Pixel (hi, lo);
    // 1 and 128: a HUD panel's, fixed to the view -- on a whole pixel, as the layout put it.
    // Half a pixel off (the middle of an odd view), its text is sampled between texels.
    if ((flags & 129u) == 129u)
        anchor = floor (anchor + 0.5);
    // 128: laid out at the view's DPI already (a HUD panel); otherwise logical pixels.
    float2 local = offset * ((flags & 128u) != 0u ? 1.0 : Surface.z);
    bool visible = true;
    if ((flags & 256u) != 0u && length (Turned (dir)) < minSpan * Surface.z)
        visible = false;
    if ((flags & 2u) != 0u)
    {
        float2 d = Turned (dir);
        float span = length (d);
        if ((flags & 16u) != 0u && span < minSpan * Surface.z)
            visible = false;
        float2 u = span > 1e-6 ? d / span : float2 (1.0, 0.0);
        if ((flags & 4u) != 0u && (u.x < -1e-4 || (abs (u.x) <= 1e-4 && u.y > 0.0)))
            u = -u;
        local = float2 (local.x * u.x - local.y * u.y, local.x * u.y + local.y * u.x);
    }
    o.position = visible ? ClipOf (anchor + local) : kCulled;
    o.uv = uv;
    o.colour = colour;
    o.halo = halo;
    // Negative: an automatic halo's scale, which the pixel shader sizes (HaloReach).
    o.haloPixels = haloPixels >= 0.0 ? haloPixels * Surface.z : haloPixels;
    o.solid = (flags & 64u) != 0u ? 2.0 : ((flags & 8u) != 0u ? 1.0 : 0.0);
    return o;
}
)hlsl";

// The 3D window: Archicad's camera through `ArchicadClip`, composed per camera slot by
// `camerashader::Compose`, which also declares `View`. Pixels are the viewport's.
inline constexpr const char* kSceneBody = R"hlsl(
cbuffer GuestSceneFrame
{
    float4 Surface;  // the viewport's width and height in pixels, the DPI scale, the depth pull
};

static const float kNearW = 1e-5;
static const float kParallelPullPixels = 1.5;

// ⚠️ TOWARD THE EYE BY A FRACTION OF THE DISTANCE, NOT A STEP IN NDC DEPTH (the note
// on kGuestDepthPullFraction, OverlayStyle.hpp, says what the step did). `c` is
// `position` through ArchicadClip. Perspective: along every ray z = A w + B, so the
// eye is (0, 0, B, 0) in clip space and a point moved toward it by the fraction f is
// ((1-f) x, (1-f) y, (1-f) z + f B, (1-f) w) -- the same pixel, nearer. A and B come
// from the camera itself, measured along the axis w changes most along (at least
// 1/sqrt 3 per metre), so no depth mapping is assumed. Parallel: no eye; the pull is
// kParallelPullPixels pixels' worth of metres along the view.
float4 TowardEye (float4 c, float3 position)
{
    float4 dx = ArchicadClip (float4 (position + float3 (1.0, 0.0, 0.0), 1.0)) - c;
    float4 dy = ArchicadClip (float4 (position + float3 (0.0, 1.0, 0.0), 1.0)) - c;
    float4 dz = ArchicadClip (float4 (position + float3 (0.0, 0.0, 1.0), 1.0)) - c;
    if (ArchicadParallel ())
    {
        float depthPerMetre = length (float3 (dx.z, dy.z, dz.z));
        float widthPerMetre = max (length (float3 (dx.x, dy.x, dz.x)), 1e-12);
        float metres = kParallelPullPixels * (2.0 / Surface.x) / widthPerMetre;
        c.z -= metres * depthPerMetre * c.w;
        return c;
    }
    float3 dw = float3 (dx.w, dy.w, dz.w);
    float3 dd = float3 (dx.z, dy.z, dz.z);
    float3 a = abs (dw);
    float slope = a.x >= a.y && a.x >= a.z ? dd.x / dw.x : (a.y >= a.z ? dd.y / dw.y : dd.z / dw.z);
    float eyeZ = c.z - slope * c.w;
    float keep = 1.0 - Surface.w;
    return float4 (c.xy * keep, c.z * keep + Surface.w * eyeZ, c.w * keep);
}

float2 ToPixels (float4 c)
{
    float2 n = c.xy / c.w;
    return float2 ((n.x * 0.5 + 0.5) * Surface.x, (0.5 - n.y * 0.5) * Surface.y);
}

float4 FromPixels (float2 p, float depth)
{
    return float4 (p.x / Surface.x * 2.0 - 1.0, 1.0 - p.y / Surface.y * 2.0, depth, 1.0);
}

FillOut VSFill (float3 position : ATTRIB0, float3 normal : ATTRIB1, float2 offset : ATTRIB2, float4 colour : ATTRIB3,
                float value : ATTRIB4)
{
    FillOut o;
    if (Mode.w > 0.5)
        o.position = FromPixels (position.xy * Surface.xy + offset * Surface.z, 0.0);
    else
    {
        float4 c = TowardEye (ArchicadClip (float4 (position, 1.0)), position);
        c.xy += offset * Surface.z * float2 (2.0 / Surface.x, -2.0 / Surface.y) * c.w;
        o.position = c;
    }
    o.colour = colour;
    o.value = value;
    o.normalView = mul (float4 (normal, 0.0), View).xyz;
    o.hatchXY = position.xy;
    return o;
}

LineOut VSLine (float3 a : ATTRIB0, float3 b : ATTRIB1, float4 colour : ATTRIB2, float4 hiddenColour : ATTRIB3,
                float3 style : ATTRIB4, uint dashes : ATTRIB5, uint behind : ATTRIB6, uint corner : SV_VertexID)
{
    LineOut o;
    // Pulled toward the eye first: the pull is affine in clip space, so the cut at the
    // eye below still interpolates a line.
    float4 ca = TowardEye (ArchicadClip (float4 (a, 1.0)), a);
    float4 cb = TowardEye (ArchicadClip (float4 (b, 1.0)), b);
    bool draw = InPass (behind) && (ca.w > kNearW || cb.w > kNearW);
    // The part behind the eye is cut away in clip space, where it is still a line.
    float ta = 0.0, tb = 1.0;
    float span = cb.w - ca.w;
    if (ca.w < kNearW && abs (span) > 1e-12)
    {
        ta = (kNearW - ca.w) / span;
        ca = lerp (ca, cb, ta);
    }
    else if (cb.w < kNearW && abs (span) > 1e-12)
    {
        tb = (kNearW - ca.w) / span;
        cb = lerp (ca, cb, tb);
    }
    float2 pa = ToPixels (ca);
    float2 pb = ToPixels (cb);
    float za = saturate (ca.z / ca.w);
    float zb = saturate (cb.z / cb.w);
    // Behind the building: the line's hidden width, colour and pattern.
    bool behindPass = Mode.z > 0.5 && Mode.z < 1.5;
    float halfWidth = (behindPass && style.y > 0.0 ? style.y : style.x) * 0.5 * Surface.z;
    float pushed = halfWidth + 1.0;
    float2 d = pb - pa;
    float len = length (d);
    float2 along = len > 1e-3 ? d / len : float2 (1.0, 0.0);
    float2 across = float2 (-along.y, along.x);
    bool atB = corner == 1 || corner == 2 || corner == 4;
    float side = (corner == 2 || corner == 4 || corner == 5) ? 1.0 : -1.0;
    float2 p = (atB ? pb + along * pushed : pa - along * pushed) + across * (side * pushed);
    o.position = draw ? FromPixels (p, atB ? zb : za) : kCulled;
    // ⚠️ THE PATTERN IS ANCHORED IN THE MODEL: metres along the polyline at each end of the
    // part in front of the eye, carried over w so the fragment divides back to its own
    // metres -- never pixels from wherever the start happens to project (OverlayLayers.hpp).
    float metres = length (b - a);
    float ma = style.z + ta * metres;
    float mb = style.z + tb * metres;
    float s = len > 1e-3 ? clamp ((atB ? len + pushed : -pushed) / len, -0.25, 1.25) : (atB ? 1.0 : 0.0);
    float ia = 1.0 / max (ca.w, kNearW);
    float ib = 1.0 / max (cb.w, kNearW);
    o.metres = float2 (lerp (ma * ia, mb * ib, s), max (lerp (ia, ib, s), 1e-12));
    o.halfWidth = halfWidth;
    o.dash = behindPass ? (dashes >> 8) & 255u : dashes & 255u;
    o.colour = colour;
    if (behindPass)
    {
        if (hiddenColour.a > 0.0)
            o.colour = hiddenColour;
        else if (behind == 1u)
            o.colour.a *= 0.3;
    }
    o.across = side * pushed;
    return o;
}

GlyphOut VSGlyph (float3 position : ATTRIB0, float3 dir : ATTRIB1, float2 offset : ATTRIB2, float2 uv : ATTRIB3,
                  float4 colour : ATTRIB4, float4 halo : ATTRIB5, float haloPixels : ATTRIB6, uint flags : ATTRIB7,
                  float minSpan : ATTRIB8)
{
    GlyphOut o;
    o.uv = uv;
    o.colour = colour;
    o.halo = halo;
    // Negative: an automatic halo's scale, which the pixel shader sizes (HaloReach).
    o.haloPixels = haloPixels >= 0.0 ? haloPixels * Surface.z : haloPixels;
    o.solid = (flags & 64u) != 0u ? 2.0 : ((flags & 8u) != 0u ? 1.0 : 0.0);
    // 32: every corner is its own model point -- text lying on a plane in the model.
    // Projected whole, so the glyph is perspective-correct and occluded by depth.
    if ((flags & 256u) != 0u) {
        float4 a = ArchicadClip (float4 (position, 1.0));
        float4 b = ArchicadClip (float4 (position + dir, 1.0));
        if (a.w <= kNearW || b.w <= kNearW || length (ToPixels (b) - ToPixels (a)) < minSpan * Surface.z) {
            o.position = kCulled;
            return o;
        }
    }
    if ((flags & 32u) != 0u)
    {
        float4 m = TowardEye (ArchicadClip (float4 (position, 1.0)), position);
        o.position = m.w > kNearW ? m : kCulled;
        return o;
    }
    bool visible = true;
    float2 anchor;
    float depth = 0.0;
    if ((flags & 1u) != 0u)
    {
        anchor = position.xy * Surface.xy;
        // 128 too: a HUD panel's -- on a whole pixel, as the layout put it (the plan's note).
        if ((flags & 128u) != 0u)
            anchor = floor (anchor + 0.5);
    }
    else
    {
        float4 c = TowardEye (ArchicadClip (float4 (position, 1.0)), position);
        visible = c.w > kNearW;
        anchor = ToPixels (c);
        depth = saturate (c.z / c.w);
    }
    float2 local = offset * ((flags & 128u) != 0u ? 1.0 : Surface.z);
    if ((flags & 2u) != 0u)
    {
        // A span's anchor is its middle and `dir` the whole span; otherwise `dir`
        // points from the anchor.
        bool whole = (flags & 16u) != 0u;
        float4 cf = ArchicadClip (float4 (whole ? position - dir * 0.5 : position, 1.0));
        float4 ct = ArchicadClip (float4 (whole ? position + dir * 0.5 : position + dir, 1.0));
        float2 u = float2 (1.0, 0.0);
        if (cf.w > kNearW && ct.w > kNearW)
        {
            float2 d = ToPixels (ct) - ToPixels (cf);
            float span = length (d);
            if (whole && span < minSpan * Surface.z)
                visible = false;
            if (span > 1e-4)
                u = d / span;
        }
        if ((flags & 4u) != 0u && (u.x < -1e-4 || (abs (u.x) <= 1e-4 && u.y > 0.0)))
            u = -u;
        local = float2 (local.x * u.x - local.y * u.y, local.x * u.y + local.y * u.x);
    }
    o.position = visible ? FromPixels (anchor + local, depth) : kCulled;
    return o;
}
)hlsl";

// The plan's whole source.
inline std::string PlanSource ()
{
    return std::string (kCommon) + kPlanBody;
}

// The 3D body, to be composed with `camerashader::Compose` for a camera slot.
inline std::string SceneBody ()
{
    return std::string (kCommon) + kSceneBody;
}

} // namespace guestshaders
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
