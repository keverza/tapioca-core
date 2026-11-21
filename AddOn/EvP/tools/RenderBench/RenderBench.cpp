// RenderBench -- the viewer's renderer outside Archicad: load a scene, render it at fixed cameras
// in several styles, and write each picture to PNG for an image comparison.
//
// âš ï¸ THE REAL RENDERER, DRIVEN THE WAY THE VIEWPORT DRIVES IT. DiligentScene, its passes and
// its shaders are the add-on's own sources compiled into this executable; the frame sequence
// below is DiligentViewport's (shadow map, ambient occlusion, Draw, AdvanceFrame) and the looks
// come from the HUD's own presets (ApplyPreview, ApplySurfaceLook). What a bench picture shows
// wrong is what the viewer shows wrong -- unless this sequence drifts from the viewport's, so
// change both or neither.
//
// The reference for every comparison is the RayTraced style left to converge: same scene,
// camera, sun and materials, so a difference from it is the raster path's own error.
//
//   TapiocaRenderBench --scene room.obj --yup --out dir --size 1280x720
//       --view "name:ex,ey,ez:tx,ty,tz:fovDeg" [--view ...] [--styles ssr,traced,reference]
//       [--sun az,alt] [--env sky.hdr] [--samples 256] [--frames 48]

#include "ObjScene.hpp"
#include "SunVisibility.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiligentDeviceFactory.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentNativeInterop.hpp"

#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentViewportSupport.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/RayTracedView.hpp"
#include "ArchViz/SceneCmdQueue.hpp"

#include <DeviceContext.h>
#include <RenderDevice.h>
#include <Texture.h>
#include <TextureView.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace av = geomsrv::archviz;

namespace geomsrv::archviz {

// The add-on's log writes through Archicad; here it is the console, and a file beside the output.
static std::ofstream g_log;
void ArchVizLog (const std::string& line)
{
    std::fprintf (stderr, "%s\n", line.c_str ());
    if (g_log)
        g_log << line << "\n";
}
void ArchVizLogClose ()
{
}

} // namespace geomsrv::archviz

namespace {

struct View {
    std::string name;
    float eye[3] = {};
    float target[3] = {};
    float fov = 50.0f;
};

struct Options {
    std::string scene;
    std::string out = ".";
    std::string env;
    bool yUp = false;
    float scale = 1.0f;
    uint32_t width = 1280, height = 720;
    std::vector<View> views;
    std::vector<std::string> styles { "mask", "ssr", "traced", "reference" };
    float sunAz = 135.0f, sunAlt = 40.0f;
    uint32_t samples = 256;
    // --rt-budget MS: the Ray traced style's time per frame (#231); 0 = one sample a frame, as before.
    float rtBudgetMs = -1.0f;
    // --rt-fade S: the Ray traced image fades in over S seconds of wall clock (#14). Default 0: the
    // bench's frames are not paced, so a held or fading image would land in the pictures by timing.
    float rtFadeSeconds = 0.0f;
    uint32_t frames = 48;
    float ssrThickness = -1.0f;
    int ssrSteps = -1;
    // --off: effects switched off in every style, to isolate one in a picture (ao, ssr, taa,
    // shadows, env).
    std::vector<std::string> off;
    // Shadow-map overrides (negative: the viewer's default), to sweep the biases.
    float shadowBias = -1.0f, shadowClamp = -1.0f, shadowFilter = -1.0f;
    int shadowPcss = -1;
    int shadowResolution = int (av::DiligentShadowSettings {}.resolution);
    int shadowFit = -1;
    int shadowDiagnostic = 0;
    float shadowSearch = -1.0f;
    float shadowSunSize = -1.0f;
    // --section axis,position[,hatchMm[,angleDeg]]: the viewer's section plane with caps, the
    // hatch on when a spacing is given (#251). position 0..1 across the model's bounds. Set as
    // HUD state: the frame applies the HUD's look (DiligentViewportLook), which owns the plane.
    // --temporal N: after the picture is taken, N more frames of the same still camera, each read
    // back and compared with the one before (#247/#248). 0: off.
    uint32_t temporal = 0;
    // --temporal-change K: from the K-th of those frames on, the white balance moves to 4500 K -- a
    // content change made while the view is converged (and idle), which must reach the picture.
    int temporalChange = -1;
    // --motion N[:dolly|orbit]: after the picture (and --temporal), N frames of a moving camera --
    // a dolly towards the target (0.3 % of the distance a frame) or an orbit about the vertical
    // through it (0.1 degree a frame) -- scored for flicker (#14): see MotionFlicker.
    uint32_t motion = 0;
    bool motionOrbit = false;
    // --edges: the technical edges on (#14 P2), the lines #248 reports moving with the jitter.
    bool edges = false;
    // --converge-settle N / --converge-samples N: the still-camera mean's settle frames and target
    // (TemporalConvergenceSettings); negative / 0 keeps the viewer's.
    int convergeSettle = -1;
    int convergeSamples = 0;
    float convergeFilter = -1.0f; // --converge-filter R: the still image's tent radius, px (#14 moire)
    bool section = false;
    int sectionAxis = 2;
    float sectionPosition = 0.5f;
    float sectionHatchMm = 0.0f, sectionHatchAngle = 45.0f;
};

bool ParseTriple (const std::string& text, float out[3])
{
    return std::sscanf (text.c_str (), "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

bool ParseView (const std::string& text, View& view)
{
    std::vector<std::string> parts;
    std::stringstream in (text);
    std::string part;
    while (std::getline (in, part, ':'))
        parts.push_back (part);
    if (parts.size () < 3)
        return false;
    view.name = parts[0];
    if (!ParseTriple (parts[1], view.eye) || !ParseTriple (parts[2], view.target))
        return false;
    if (parts.size () > 3)
        view.fov = std::stof (parts[3]);
    return true;
}

bool ParseArgs (int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] () -> std::string { return i + 1 < argc ? argv[++i] : std::string (); };
        if (a == "--scene")
            o.scene = next ();
        else if (a == "--out")
            o.out = next ();
        else if (a == "--env")
            o.env = next ();
        else if (a == "--yup")
            o.yUp = true;
        else if (a == "--scale")
            o.scale = std::stof (next ());
        else if (a == "--size") {
            if (std::sscanf (next ().c_str (), "%ux%u", &o.width, &o.height) != 2)
                return false;
        }
        else if (a == "--view") {
            View v;
            if (!ParseView (next (), v))
                return false;
            o.views.push_back (v);
        }
        else if (a == "--styles") {
            o.styles.clear ();
            std::stringstream in (next ());
            std::string s;
            while (std::getline (in, s, ','))
                o.styles.push_back (s);
        }
        else if (a == "--sun") {
            if (std::sscanf (next ().c_str (), "%f,%f", &o.sunAz, &o.sunAlt) != 2)
                return false;
        }
        else if (a == "--samples")
            o.samples = uint32_t (std::stoul (next ()));
        else if (a == "--rt-budget")
            o.rtBudgetMs = std::stof (next ());
        else if (a == "--rt-fade")
            o.rtFadeSeconds = std::stof (next ());
        else if (a == "--frames")
            o.frames = uint32_t (std::stoul (next ()));
        else if (a == "--temporal")
            o.temporal = uint32_t (std::stoul (next ()));
        else if (a == "--motion") {
            const std::string m = next ();
            const size_t colon = m.find (':');
            o.motion = uint32_t (std::stoul (m.substr (0, colon)));
            o.motionOrbit = colon != std::string::npos && m.substr (colon + 1) == "orbit";
        }
        else if (a == "--temporal-change")
            o.temporalChange = std::stoi (next ());
        else if (a == "--converge-settle")
            o.convergeSettle = std::stoi (next ());
        else if (a == "--converge-samples")
            o.convergeSamples = std::stoi (next ());
        else if (a == "--converge-filter")
            o.convergeFilter = std::stof (next ());
        else if (a == "--ssr-thickness")
            o.ssrThickness = std::stof (next ());
        else if (a == "--ssr-steps")
            o.ssrSteps = std::stoi (next ());
        else if (a == "--shadow-bias")
            o.shadowBias = std::stof (next ());
        else if (a == "--shadow-clamp")
            o.shadowClamp = std::stof (next ());
        else if (a == "--shadow-filter")
            o.shadowFilter = std::stof (next ());
        else if (a == "--shadow-pcss")
            o.shadowPcss = std::stoi (next ());
        else if (a == "--shadow-resolution")
            o.shadowResolution = std::stoi (next ());
        else if (a == "--shadow-sun-size")
            o.shadowSunSize = std::stof (next ());
        else if (a == "--shadow-search")
            o.shadowSearch = std::stof (next ());
        else if (a == "--shadow-diag")
            o.shadowDiagnostic = std::stoi (next ());
        else if (a == "--edges")
            o.edges = true;
        else if (a == "--section") {
            char axis = 0;
            const int n = std::sscanf (next ().c_str (), " %c,%f,%f,%f", &axis, &o.sectionPosition, &o.sectionHatchMm,
                                       &o.sectionHatchAngle);
            if (n < 2 || (axis != 'x' && axis != 'y' && axis != 'z'))
                return false;
            o.section = true;
            o.sectionAxis = axis - 'x'; // the HUD's 0 = X, 1 = Y, 2 = Z
        }
        else if (a == "--shadow-fit")
            o.shadowFit = std::stoi (next ());
        else if (a == "--off") {
            std::stringstream in (next ());
            std::string s;
            while (std::getline (in, s, ','))
                o.off.push_back (s);
        }
        else
            return false;
    }
    return !o.scene.empty ();
}

// The HUD state a style starts from: the viewer's defaults, then the style's preset.
//
// ⚠️ A COPY OF kPresets IN DiligentHudRenderTab.cpp (Architecture, Rendered, RayTraced rows), not
// a call to ApplyPreview: that TU links the whole HUD -- editors, massing, consoles -- and the
// bench is the renderer only. Change both or neither.
av::HudState StyleState (const std::string& style, const Options& o)
{
    av::HudState hud;
    hud.renderMode = int (av::SceneRenderMode::Shaded);
    hud.renderQuality = int (av::RenderQuality::Realistic);
    hud.environmentEnabled = true;
    hud.ambientOcclusion = true;
    hud.shadowsEnabled = true;
    hud.clay = false;
    hud.technicalEdges = o.edges;
    if (o.section) {
        hud.sectionEnabled = true;
        hud.sectionAxis = o.sectionAxis;
        hud.sectionPosition = o.sectionPosition;
        hud.sectionHatch = o.sectionHatchMm > 0.0f;
        if (hud.sectionHatch) {
            hud.sectionHatchSpacingMm = o.sectionHatchMm;
            hud.sectionHatchAngle = o.sectionHatchAngle;
        }
    }
    if (style == "architecture") {
        hud.ambientOcclusionIntensity = 1.4f;
        hud.environmentBackground = false;
        hud.screenSpaceReflection = false;
        hud.temporalAntiAliasing = true;  // #14: TAA against the moire...
        hud.rayTracedReflections = false; // ...without traced light, which darkened the look
        hud.rayTraced = false;
    }
    else {
        hud.ambientOcclusionIntensity = 0.8f;
        hud.environmentBackground = true;
        const bool reference = style == "reference";
        hud.screenSpaceReflection = !reference;
        hud.temporalAntiAliasing = !reference;
        hud.rayTraced = reference;
        hud.rayTracedMaxSamples = int (o.samples);
        if (o.rtBudgetMs >= 0.0f)
            hud.rayTracedBudgetMs = o.rtBudgetMs;
        hud.rayTracedHoldSeconds = 0.0f;
        hud.rayTracedFadeSeconds = o.rtFadeSeconds;
        // "ssr" is the fallback every non-DXR machine sees; "traced" is Rendered on DXR.
        hud.rayTracedReflections = style == "traced";
    }
    if (style == "clay") {
        // kPresets' Clay row: Architecture's light over the clay material, from Rendered (TAA on).
        hud.ambientOcclusionIntensity = 1.4f;
        hud.environmentBackground = false;
        hud.clay = true;
    }
    if (style == "technical") {
        // kPresets' Technical row: clay under the edges in the flat Fast light, no AO or shadows;
        // TAA and reflections are left as the previous preset had them (here Rendered's).
        hud.renderQuality = int (av::RenderQuality::Fast);
        hud.ambientOcclusion = false;
        hud.shadowsEnabled = false;
        hud.clay = true;
        hud.technicalEdges = true;
    }
    if (style == "shadow") {
        // The raster sun visibility alone (DiligentShadowMap's shadowsOnly view): what the
        // CPU-traced "sunvis" picture is compared against (#234). Nothing that blurs or adds
        // light, so a pixel is the shadow map's answer for that pixel and nothing else.
        hud.shadowOnly = true;
        hud.ambientOcclusion = false;
        hud.screenSpaceReflection = false;
        hud.temporalAntiAliasing = false;
        hud.environmentBackground = false;
    }
    if (style == "mask") {
        // The G-buffer's material data (roughness, metalness, reflectance): compare.py's glossy mask.
        hud.debugView = 12;
        hud.temporalAntiAliasing = false;
        hud.screenSpaceReflection = false;
    }
    if (o.ssrThickness > 0.0f)
        hud.ssrThickness = o.ssrThickness;
    if (o.ssrSteps > 0)
        hud.ssrMaxSteps = o.ssrSteps;
    for (const std::string& effect : o.off) {
        if (effect == "ao")
            hud.ambientOcclusion = false;
        else if (effect == "ssr")
            hud.screenSpaceReflection = false;
        else if (effect == "taa")
            hud.temporalAntiAliasing = false;
        else if (effect == "shadows")
            hud.shadowsEnabled = false;
        else if (effect == "env")
            hud.environmentEnabled = false;
        else if (effect == "converge") // #247/#248: TAA alone, as before the still-camera mean
            hud.temporalConvergence = false;
        else if (effect == "overlays") // #248: the passes after the resolve keep the jitter
            hud.stableOverlays = false;
        else if (effect == "idle") // #247: a converged view keeps rendering every pass
            hud.idleWhenConverged = false;
        else if (effect == "textures") // #252: surfaces in their colour, as before textures
            hud.materialTextures = false;
    }
    if (o.shadowBias >= 0.0f)
        hud.shadowDepthBias = o.shadowBias;
    if (o.shadowClamp >= 0.0f)
        hud.shadowReceiverBiasClamp = o.shadowClamp;
    if (o.shadowPcss >= 0)
        hud.shadowPcss = o.shadowPcss != 0;
    if (o.shadowFilter >= 0.0f)
        hud.shadowFilterWorldSize = o.shadowFilter;
    hud.sunOverride = true;
    hud.sunAzimuthDegrees = o.sunAz;
    hud.sunAltitudeDegrees = o.sunAlt;
    return hud;
}

struct Targets {
    Diligent::RefCntAutoPtr<Diligent::ITexture> color;
    Diligent::RefCntAutoPtr<Diligent::ITexture> depth;
    Diligent::RefCntAutoPtr<Diligent::ITexture> staging;
};

bool CreateTargets (Diligent::IRenderDevice* device, uint32_t w, uint32_t h, Targets& t)
{
    Diligent::TextureDesc d;
    d.Type = Diligent::RESOURCE_DIM_TEX_2D;
    d.Width = w;
    d.Height = h;
    d.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    d.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
    d.Name = "bench colour";
    device->CreateTexture (d, nullptr, &t.color);
    d.Format = Diligent::TEX_FORMAT_D32_FLOAT;
    d.BindFlags = Diligent::BIND_DEPTH_STENCIL | Diligent::BIND_SHADER_RESOURCE;
    d.Name = "bench depth";
    device->CreateTexture (d, nullptr, &t.depth);
    d.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    d.BindFlags = Diligent::BIND_NONE;
    d.Usage = Diligent::USAGE_STAGING;
    d.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    d.Name = "bench readback";
    device->CreateTexture (d, nullptr, &t.staging);
    return t.color != nullptr && t.depth != nullptr && t.staging != nullptr;
}

// The colour target as tightly packed RGBA8 rows; empty if the readback failed.
std::vector<uint8_t> ReadBack (Diligent::IDeviceContext* context, Targets& t, uint32_t w, uint32_t h)
{
    Diligent::CopyTextureAttribs copy (t.color, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, t.staging,
                                       Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture (copy);
    context->WaitForIdle ();
    Diligent::MappedTextureSubresource mapped;
    context->MapTextureSubresource (t.staging, 0, 0, Diligent::MAP_READ, Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr,
                                    mapped);
    if (mapped.pData == nullptr)
        return {};
    std::vector<uint8_t> rows (size_t (w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy (&rows[size_t (y) * w * 4], static_cast<const uint8_t*> (mapped.pData) + size_t (y) * mapped.Stride,
                     size_t (w) * 4);
    context->UnmapTextureSubresource (t.staging, 0, 0);
    return rows;
}

bool SavePng (Diligent::IDeviceContext* context, Targets& t, uint32_t w, uint32_t h, const std::string& path)
{
    const std::vector<uint8_t> rows = ReadBack (context, t, w, h);
    return !rows.empty () && stbi_write_png (path.c_str (), int (w), int (h), 4, rows.data (), int (w) * 4) != 0;
}

// What changed between two consecutive frames of a still camera (#247/#248): the pixels whose
// largest channel moved by more than `threshold` levels of 255, and the largest move anywhere.
struct FrameChange {
    size_t pixels = 0;
    int largest = 0;
};

FrameChange CompareFrames (const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int threshold)
{
    FrameChange change;
    if (a.size () != b.size ())
        return change;
    for (size_t p = 0; p + 3 < a.size (); p += 4) {
        int worst = 0;
        for (size_t c = 0; c < 3; ++c)
            worst = std::max (worst, std::abs (int (a[p + c]) - int (b[p + c])));
        change.largest = std::max (change.largest, worst);
        if (worst > threshold)
            ++change.pixels;
    }
    return change;
}

// #14: how much a moving camera's picture flickers. Each frame of the path is also rendered
// kMotionSupersample x larger with TAA off and box-filtered down: what the pixel should be. A
// frame's error against that is its aliasing; how much the error CHANGES from one frame to the next
// is the flicker -- moire and edge shimmer, with the content's own motion taken out (a fine pattern
// sliding across the screen changes every pixel, anti-aliased or not; its error does not).
// `mean` is that change in levels over every pixel; `pixels` counts those above `threshold`.
constexpr uint32_t kMotionSupersample = 4;

struct Flicker {
    double mean = 0.0;
    size_t pixels = 0;
    double error = 0.0; // this frame's mean |frame - reference|, levels
};

Flicker MotionFlicker (const std::vector<uint8_t>& previous, const std::vector<uint8_t>& previousReference,
                       const std::vector<uint8_t>& current, const std::vector<uint8_t>& reference, int threshold)
{
    Flicker f;
    const size_t n = current.size ();
    if (n == 0 || reference.size () != n || previous.size () != n || previousReference.size () != n)
        return f;
    double sum = 0.0, error = 0.0;
    for (size_t p = 0; p + 3 < n; p += 4) {
        int worst = 0, worstError = 0;
        for (size_t k = 0; k < 3; ++k) {
            const int now = int (current[p + k]) - int (reference[p + k]);
            const int before = int (previous[p + k]) - int (previousReference[p + k]);
            worst = std::max (worst, std::abs (now - before));
            worstError = std::max (worstError, std::abs (now));
        }
        sum += worst;
        error += worstError;
        if (worst > threshold)
            ++f.pixels;
    }
    f.mean = sum / double (n / 4);
    f.error = error / double (n / 4);
    return f;
}

// `factor` x `factor` boxes of an RGBA image `w` x `h` (its size, not the result's).
std::vector<uint8_t> BoxDown (const std::vector<uint8_t>& image, uint32_t w, uint32_t h, uint32_t factor)
{
    const uint32_t ow = w / factor, oh = h / factor;
    std::vector<uint8_t> out (size_t (ow) * oh * 4, 255);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x)
            for (uint32_t c = 0; c < 3; ++c) {
                uint32_t sum = 0;
                for (uint32_t dy = 0; dy < factor; ++dy)
                    for (uint32_t dx = 0; dx < factor; ++dx)
                        sum += image[(size_t (y * factor + dy) * w + x * factor + dx) * 4 + c];
                out[(size_t (y) * ow + x) * 4 + c] = uint8_t ((sum + factor * factor / 2) / (factor * factor));
            }
    return out;
}

// The camera `k` frames into --motion.
View MovedView (const View& view, const Options& o, uint32_t k)
{
    View moved = view;
    float d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = view.eye[i] - view.target[i];
    if (o.motionOrbit) {
        const float angle = 0.1f * float (k) * 3.14159265f / 180.0f;
        const float c = std::cos (angle), s = std::sin (angle);
        moved.eye[0] = view.target[0] + c * d[0] - s * d[1];
        moved.eye[1] = view.target[1] + s * d[0] + c * d[1];
    }
    else {
        const float scale = std::pow (0.997f, float (k));
        for (int i = 0; i < 3; ++i)
            moved.eye[i] = view.target[i] + d[i] * scale;
    }
    return moved;
}

// One frame, in DiligentViewport's order. `frame` drives TAA's jitter sequence and the tracer's
// sample index.
void RenderFrame (av::DiligentScene& scene, Diligent::IDeviceContext* context, Targets& t, const Options& o,
                  const View& view, const av::HudState& hud, uint32_t frame)
{
    const uint32_t w = o.width, h = o.height;
    float view16[16], stableProj[16], proj[16], viewProj[16], motionViewProj[16];
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    av::LookAtRH (view16, view.eye, view.target, up);
    const float nearClip = 0.05f;
    float dx = view.target[0] - view.eye[0], dy = view.target[1] - view.eye[1], dz = view.target[2] - view.eye[2];
    const float focus = std::sqrt (dx * dx + dy * dy + dz * dz);
    const float farClip = 2000.0f;
    av::PerspectiveRH (stableProj, view.fov, float (w) / float (h), nearClip, farClip);
    float jitter[2] = { 0.0f, 0.0f };

    Diligent::ITextureView* rtv = t.color->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    Diligent::ITextureView* dsv = t.depth->GetDefaultView (Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    const float clear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    context->SetRenderTargets (1, &rtv, dsv, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearRenderTarget (rtv, clear, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil (dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    scene.SetViewportSize (w, h);
    const bool taa = hud.temporalAntiAliasing; // #14: Fast too, as DiligentViewport.cpp
    scene.SetTemporalAntiAliasing (taa, hud.taaStability);
    av::TemporalConvergenceSettings convergence;
    convergence.enabled = hud.temporalConvergence;
    convergence.stableOverlays = hud.stableOverlays;
    convergence.idleWhenConverged = hud.idleWhenConverged;
    if (o.convergeSettle >= 0)
        convergence.settleFrames = uint32_t (o.convergeSettle);
    if (o.convergeSamples > 0)
        convergence.targetSamples = uint32_t (o.convergeSamples);
    if (o.convergeFilter >= 0.0f)
        convergence.filterRadiusPixels = o.convergeFilter;
    scene.SetTemporalConvergence (convergence);
    scene.PrepareTemporalAntiAliasingFrame (context, frame, view16, stableProj, proj, jitter);
    av::Multiply (motionViewProj, view16, stableProj);
    av::Multiply (viewProj, view16, proj);

    scene.SetShadowsEnabled (hud.shadowsEnabled);
    // The shadow fields the bench can sweep; the rest stay at the settings' defaults, which are
    // the HUD's (DiligentViewportSupport's ApplyShadowSettings maps the same fields).
    av::DiligentShadowSettings shadow;
    shadow.fixedDepthBias = hud.shadowDepthBias;
    shadow.receiverPlaneBiasClamp = hud.shadowReceiverBiasClamp;
    shadow.pcssEnabled = hud.shadowPcss;
    shadow.filterWorldSize = hud.shadowFilterWorldSize;
    shadow.shadowsOnly = hud.shadowOnly;
    shadow.diagnostic = o.shadowDiagnostic;
    if (o.shadowSearch >= 0.0f)
        shadow.pcssBlockerSearch = o.shadowSearch;
    if (o.shadowSunSize >= 0.0f)
        shadow.pcssLightAngularDiameter = o.shadowSunSize;
    shadow.resolution = uint32_t (o.shadowResolution);
    if (o.shadowFit >= 0)
        shadow.fitCascadesToScene = o.shadowFit != 0;
    scene.SetShadowSettings (shadow);
    scene.RenderShadowMap (context, view16, stableProj);
    scene.SetSunOverride (hud.sunOverride, hud.sunAzimuthDegrees, hud.sunAltitudeDegrees);
    scene.SetEnvironmentSettings (hud.environmentEnabled, hud.environmentIntensity, hud.environmentRotationDegrees);
    scene.SetEnvironmentBackground (hud.environmentBackground);
    scene.SetSunWithSkyWeight (hud.sunWithSkyWeight);
    av::ApplySurfaceLook (scene, hud, false);
    // ⚠️ AGAIN AFTER THE LOOK, which sets the HUD's convergence settings: without this the traced passes,
    // which read them while drawing, never saw --converge-filter/-settle/-samples (#14).
    scene.SetTemporalConvergence (convergence);
    scene.SetAutoExposure (hud.autoExposure);
    scene.SetWhiteBalance (hud.whiteBalanceKelvin, hud.whiteBalanceTint);
    scene.SetAtmosphere (hud.epipolarAtmosphere, hud.atmosphereIntensity, hud.atmosphereLightShafts,
                         hud.atmosphereLightingOnly);
    scene.SetRenderMode (static_cast<av::SceneRenderMode> (hud.renderMode));
    scene.SetWireframeSettings (float (hud.wireTessellation), hud.wireLineWidth);
    scene.SetRenderQuality (hud.renderQuality == int (av::RenderQuality::Realistic) ? av::RenderQuality::Realistic
                                                                                    : av::RenderQuality::Fast);
    // The camera's rays, as SetDiligentCameraRays derives them: right, up, forward at the frustum's
    // edges (the sky and atmosphere read them).
    {
        float f[3] = { dx / focus, dy / focus, dz / focus };
        float r[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
        const float rl = std::sqrt (r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        for (float& c : r)
            c /= rl;
        float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
        const float tanY = std::tan (view.fov * 0.5f * 3.14159265f / 180.0f);
        const float tanX = tanY * float (w) / float (h);
        for (int k = 0; k < 3; ++k) {
            r[k] *= tanX;
            u[k] *= tanY;
        }
        scene.SetCameraRays (r, u, f);
    }

    scene.ClearAmbientOcclusion ();
    if (hud.debugView != 0) {
        // The G-buffer debug views (the "mask" style), drawn as DrawSceneOrDebugView draws them.
        scene.DrawGBufferDebug (context, rtv, view16, proj, viewProj, view.eye, nearClip, farClip, focus, true, frame,
                                av::CullMode::Cw, hud.debugView);
        scene.AdvanceFrame (motionViewProj);
        av::EndUnpresentedFrame (context);
        return;
    }
    scene.SetAmbientOcclusion (hud.ambientOcclusion, hud.ambientOcclusionIntensity, hud.ambientOcclusionRadius);
    scene.SetScreenSpaceReflection (hud.screenSpaceReflection, hud.ssrIntensity, hud.ssrRoughnessThreshold);
    scene.SetTemporalAntiAliasing (taa, hud.taaStability);
    scene.PrepareAmbientOcclusion (context, view16, proj, viewProj, motionViewProj, view.eye, jitter, nearClip, farClip,
                                   focus, frame, av::CullMode::Cw);
    scene.Draw (context, rtv, dsv, view16, proj, viewProj, motionViewProj, view.eye, jitter, av::CullMode::Cw, 0,
                nearClip, farClip, focus, frame);
    scene.AdvanceFrame (motionViewProj);
    // ⚠️ AN UNPRESENTED FRAME MUST STILL END: without FinishFrame, D3D12 recycles no descriptors and
    // the GPU heap runs out a hundred frames in (DiligentNativeInterop.hpp, pitfall 2).
    av::EndUnpresentedFrame (context);
}

// A crash prints its stack, symbolised from the PDB beside the executable: the bench runs unattended,
// and with /FORCE:UNRESOLVED a call into the forced tail is exactly a crash at address zero.
LONG WINAPI PrintCrash (EXCEPTION_POINTERS* info)
{
    HANDLE process = GetCurrentProcess ();
    SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize (process, nullptr, TRUE);
    std::fprintf (stderr, "bench: CRASH 0x%08lX at %p\n", info->ExceptionRecord->ExceptionCode,
                  info->ExceptionRecord->ExceptionAddress);
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0; depth < 40; ++depth) {
        if (!StackWalk64 (IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread (), &frame, &context, nullptr,
                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
            break;
        alignas (SYMBOL_INFO) char buffer[sizeof (SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*> (buffer);
        symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 offset = 0;
        IMAGEHLP_LINE64 line {};
        line.SizeOfStruct = sizeof (line);
        DWORD lineOffset = 0;
        const bool named = SymFromAddr (process, frame.AddrPC.Offset, &offset, symbol) != FALSE;
        const bool lined = SymGetLineFromAddr64 (process, frame.AddrPC.Offset, &lineOffset, &line) != FALSE;
        std::fprintf (stderr, "  #%d %s %s:%lu\n", depth, named ? symbol->Name : "?", lined ? line.FileName : "",
                      lined ? line.LineNumber : 0ul);
    }
    std::fflush (stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

int main (int argc, char** argv)
{
    SetUnhandledExceptionFilter (&PrintCrash);
    Options o;
    if (!ParseArgs (argc, argv, o)) {
        std::fprintf (stderr, "usage: TapiocaRenderBench --scene file.obj [--yup] [--scale s] --out dir "
                              "[--size WxH] --view name:ex,ey,ez:tx,ty,tz[:fov] ... [--styles ssr,traced,reference] "
                              "[--sun az,alt] [--env sky.hdr] [--samples N] [--frames N] "
                              "[--section x|y|z,pos[,hatchMm[,angle]]]\n");
        return 2;
    }
    av::g_log.open (o.out + "/bench.log");

    const auto t0 = std::chrono::steady_clock::now ();
    renderbench::ObjScene obj;
    std::string error;
    if (!renderbench::LoadObjScene (o.scene, o.yUp, o.scale, obj, error)) {
        av::ArchVizLog ("bench: " + error);
        return 1;
    }
    const double loadSeconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
    av::ArchVizLog ("bench: " + std::to_string (obj.triangles) + " triangles, " +
                    std::to_string (obj.elements.size ()) + " materials in use (" + std::to_string (obj.textured) +
                    " textured), bounds " + std::to_string (obj.boundsMin[0]) + "," +
                    std::to_string (obj.boundsMin[1]) + "," + std::to_string (obj.boundsMin[2]) + " .. " +
                    std::to_string (obj.boundsMax[0]) + "," + std::to_string (obj.boundsMax[1]) + "," +
                    std::to_string (obj.boundsMax[2]) + ", loaded in " + std::to_string (loadSeconds) + " s");
    for (size_t i = 0; i < obj.materialNames.size (); ++i) {
        const av::SurfaceMaterial& m = obj.materials->Lookup (int32_t (i));
        av::ArchVizLog ("bench: material " + std::to_string (i) + " '" + m.name + "' rgb " + std::to_string (m.r) +
                        "," + std::to_string (m.g) + "," + std::to_string (m.b) + " alpha " + std::to_string (m.alpha) +
                        " shine " + std::to_string (m.shininess) + " specular " + std::to_string (m.specular) +
                        " -> roughness " + std::to_string (av::SurfaceRoughness (m)));
    }

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    av::RenderBackendChoice choice;
    if (!av::CreateViewportDevice (av::RenderBackendPreference::Auto, av::ViewportBackendConstraints (false), device,
                                   context, choice, error)) {
        av::ArchVizLog ("bench: no device: " + error);
        return 1;
    }
    Targets targets;
    if (!CreateTargets (device, o.width, o.height, targets)) {
        av::ArchVizLog ("bench: could not create the render targets");
        return 1;
    }
    av::DiligentScene scene;
    if (!scene.Init (device, uint32_t (Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB),
                     uint32_t (Diligent::TEX_FORMAT_D32_FLOAT), error)) {
        av::ArchVizLog ("bench: scene init failed: " + error);
        return 1;
    }

    renderbench::SunVisibility sunVisibility;
    if (std::find (o.styles.begin (), o.styles.end (), "sunvis") != o.styles.end () ||
        std::find (o.styles.begin (), o.styles.end (), "sunsoft") != o.styles.end ())
        sunVisibility.Build (obj); // before the elements are handed to the queue

    av::SceneCmdQueue& queue = av::SceneCmdQueue::Get ();
    queue.SetConsumer (true);
    queue.PushMaterials (std::move (obj.materials));
    queue.PushBeginBatch (true);
    for (auto& element : obj.elements)
        queue.PushUpsert (std::move (element));
    queue.PushEndBatch ();
    while (scene.Consume (device, 64, context) > 0) {
    }
    if (!o.env.empty ())
        scene.SetEnvironmentMap (o.env.c_str ());

    if (o.views.empty ()) {
        View v;
        v.name = "home";
        for (int k = 0; k < 3; ++k)
            v.target[k] = 0.5f * (obj.boundsMin[k] + obj.boundsMax[k]);
        const float span = std::max ({ obj.boundsMax[0] - obj.boundsMin[0], obj.boundsMax[1] - obj.boundsMin[1],
                                       obj.boundsMax[2] - obj.boundsMin[2] });
        v.eye[0] = v.target[0] + span * 0.9f;
        v.eye[1] = v.target[1] - span * 1.2f;
        v.eye[2] = v.target[2] + span * 0.5f;
        o.views.push_back (v);
    }

    std::ofstream report (o.out + "/bench.json");
    // Forward slashes: a Windows path's backslashes are JSON escapes, and the report must parse.
    std::string scenePath = o.scene;
    std::replace (scenePath.begin (), scenePath.end (), '\\', '/');
    report << "{\n  \"scene\": \"" << scenePath << "\",\n  \"adapter\": \"" << choice.adapterName
           << "\",\n  \"triangles\": " << obj.triangles << ",\n  \"renders\": [\n";
    bool firstRender = true;
    for (const View& view : o.views) {
        for (const std::string& style : o.styles) {
            if (style == "sunvis" || style == "sunsoft") {
                // CPU-traced, so it needs no GPU ray tracing (SunVisibility.hpp). The sun as
                // DiligentSceneImpl::EffectiveSun derives it from the override.
                const float az = o.sunAz * 3.14159265f / 180.0f, alt = o.sunAlt * 3.14159265f / 180.0f;
                const float sun[3] = { std::cos (alt) * std::cos (az), std::cos (alt) * std::sin (az), std::sin (alt) };
                const auto start = std::chrono::steady_clock::now ();
                // sunsoft: the sun as the disk the PCSS path assumes (--shadow-sun-size).
                const float diameter = style == "sunvis" ? 0.0f
                                       : o.shadowSunSize >= 0.0f
                                           ? o.shadowSunSize
                                           : av::DiligentShadowSettings {}.pcssLightAngularDiameter;
                const std::vector<uint8_t> vis =
                    sunVisibility.Render (view.eye, view.target, view.fov, o.width, o.height, sun, diameter);
                const double seconds =
                    std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
                const std::string file = view.name + "_" + style + ".png";
                const bool saved = stbi_write_png ((o.out + "/" + file).c_str (), int (o.width), int (o.height), 1,
                                                   vis.data (), int (o.width)) != 0;
                av::ArchVizLog ("bench: " + file + (saved ? " saved" : " NOT saved") + " in " +
                                std::to_string (seconds) + " s (CPU)");
                report << (firstRender ? "" : ",\n") << "    { \"view\": \"" << view.name << "\", \"style\": \""
                       << style << "\", \"file\": \"" << file << "\", \"seconds\": " << seconds << " }";
                firstRender = false;
                continue;
            }
            const av::HudState hud = StyleState (style, o);
            scene.ResetTemporalAntiAliasingHistory ();
            const bool reference = style == "reference";
            const uint32_t frames = reference ? o.samples + 8 : (style == "mask" ? 2u : o.frames);
            const auto start = std::chrono::steady_clock::now ();
            // #231: with the time budget a reference converges in far fewer frames than samples.
            // Checked every frame, so `framesRun` is the frame count to converge, the figure the
            // budget is for (the bench draws without vsync, so its seconds are not a viewer's).
            uint32_t framesRun = 0;
            for (uint32_t f = 0; f < frames; ++f) {
                RenderFrame (scene, context, targets, o, view, hud, f);
                framesRun = f + 1;
                if (reference) {
                    const av::RayTracedStats s = scene.RayTracedStatus ();
                    if (s.state == av::RayTracedState::Converged || s.state == av::RayTracedState::Unavailable)
                        break;
                }
            }
            // The caps are cut on a worker: keep rendering until the current plane's build has landed,
            // so the picture is never the back faces standing in for caps that were still coming.
            uint32_t nextFrame = frames;
            for (int extra = 0; o.section && extra < 300; ++extra) {
                const av::SectionCapCounts caps = scene.Stats ().sectionCaps;
                if (!caps.building && extra >= 2) {
                    av::ArchVizLog ("bench: section caps: " + std::to_string (caps.elementsCut) + " elements cut, " +
                                    std::to_string (caps.closedLoops) + " closed, " + std::to_string (caps.openChains) +
                                    " open, " + std::to_string (caps.fillFailures) + " refused");
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
                RenderFrame (scene, context, targets, o, view, hud, nextFrame++);
            }
            context->WaitForIdle ();
            const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
            const std::string file = view.name + "_" + style + ".png";
            const bool saved = SavePng (context, targets, o.width, o.height, o.out + "/" + file);

            // --temporal: the still camera's frame-to-frame change (#247/#248). 24 levels is the
            // threshold the #248 measurements used; "settled" is the first pair from which every
            // later pair changes nothing at all.
            std::string temporalJson;
            if (o.temporal > 0 && style != "reference") {
                constexpr int kThreshold = 24;
                std::vector<uint8_t> previous = ReadBack (context, targets, o.width, o.height);
                FrameChange worst, last;
                size_t worstAtOrAfter16 = 0;
                int settledAt = -1;
                // #247: frames that presented the mean without drawing the scene, and the first one.
                uint32_t idleFrames = 0;
                int firstIdle = -1;
                // One row per frame: a slow drift or a long-period swing (a history that never
                // settles) shows here, where the pair-to-pair counts above miss it.
                std::ofstream csv (o.out + "/" + view.name + "_" + style + "_temporal.csv");
                csv << "frame,changed,largest,mean,idle\n";
                av::HudState changed = hud;
                changed.whiteBalanceKelvin = 4500.0f;
                for (uint32_t k = 0; k < o.temporal; ++k) {
                    const bool change = o.temporalChange >= 0 && int (k) >= o.temporalChange;
                    RenderFrame (scene, context, targets, o, view, change ? changed : hud, nextFrame++);
                    std::vector<uint8_t> current = ReadBack (context, targets, o.width, o.height);
                    last = CompareFrames (previous, current, kThreshold);
                    double sum = 0.0;
                    for (size_t p = 0; p + 3 < current.size (); p += 4)
                        sum += double (current[p]) + current[p + 1] + current[p + 2];
                    const bool idle = scene.Stats ().temporal.idle;
                    if (idle && firstIdle < 0)
                        firstIdle = int (k);
                    idleFrames += idle ? 1u : 0u;
                    csv << (nextFrame - 1) << "," << last.pixels << "," << last.largest << ","
                        << sum / (3.0 * double (o.width) * o.height) << "," << (idle ? 1 : 0) << "\n";
                    if (last.pixels > worst.pixels || (last.pixels == worst.pixels && last.largest > worst.largest))
                        worst = last;
                    if (k >= 16)
                        worstAtOrAfter16 = std::max (worstAtOrAfter16, last.pixels);
                    if (last.largest == 0) {
                        if (settledAt < 0)
                            settledAt = int (k);
                    }
                    else
                        settledAt = -1;
                    previous = std::move (current);
                }
                // The still picture at the end, for comparing what a still camera settles on.
                stbi_write_png ((o.out + "/" + view.name + "_" + style + "_still.png").c_str (), int (o.width),
                                int (o.height), 4, previous.data (), int (o.width) * 4);
                const av::DiligentSceneStats st = scene.Stats ();
                av::ArchVizLog (
                    "bench: temporal " + view.name + "_" + style + ": " + std::to_string (o.temporal) +
                    " frames, worst pair " + std::to_string (worst.pixels) + " px > " + std::to_string (kThreshold) +
                    " (largest " + std::to_string (worst.largest) + "), worst after 16 frames " +
                    std::to_string (worstAtOrAfter16) + " px, last pair " + std::to_string (last.pixels) +
                    " px (largest " + std::to_string (last.largest) + "), settled " +
                    (settledAt < 0 ? std::string ("never") : "at +" + std::to_string (settledAt)) + ", convergence " +
                    av::TemporalPhaseName (st.temporal.phase) + " " + std::to_string (st.temporal.samples) + "/" +
                    std::to_string (st.temporal.targetSamples) + ", idle " + std::to_string (idleFrames) + " frames" +
                    (firstIdle < 0 ? std::string () : " from +" + std::to_string (firstIdle)) + ", restarts " +
                    std::to_string (st.temporal.restarts) + " (last " +
                    av::TemporalRestartName (st.temporal.lastRestart) + ")");
                temporalJson = ", \"temporal\": { \"frames\": " + std::to_string (o.temporal) +
                               ", \"worstPixels\": " + std::to_string (worst.pixels) +
                               ", \"worstLargest\": " + std::to_string (worst.largest) +
                               ", \"worstAfter16\": " + std::to_string (worstAtOrAfter16) +
                               ", \"lastPixels\": " + std::to_string (last.pixels) +
                               ", \"lastLargest\": " + std::to_string (last.largest) +
                               ", \"settledAt\": " + std::to_string (settledAt) + " }";
            }
            // --motion: the moving camera's flicker (#14), one row per frame (see MotionFlicker).
            if (o.motion > 2 && style != "reference" && style != "mask") {
                constexpr int kThreshold = 24;
                // 1. The references: the same path, kMotionSupersample x larger, TAA off.
                Options big = o;
                big.width = o.width * kMotionSupersample;
                big.height = o.height * kMotionSupersample;
                av::HudState still = hud;
                still.temporalAntiAliasing = false;
                // No edges in the reference: at its size their width and density fade differ from the
                // frame's, so they would read as error. Without them a steady edge is a steady error
                // (no flicker), and only edges that shimmer score.
                still.technicalEdges = false;
                std::vector<std::vector<uint8_t>> references;
                Targets bigTargets;
                if (CreateTargets (device, big.width, big.height, bigTargets)) {
                    for (uint32_t k = 0; k < o.motion; ++k) {
                        RenderFrame (scene, context, bigTargets, big, MovedView (view, o, k), still, nextFrame++);
                        references.push_back (BoxDown (ReadBack (context, bigTargets, big.width, big.height), big.width,
                                                       big.height, kMotionSupersample));
                    }
                }
                // 2. The style along the path, after a still start long enough for TAA to settle.
                scene.ResetTemporalAntiAliasingHistory ();
                for (uint32_t warm = 0; warm < 16; ++warm)
                    RenderFrame (scene, context, targets, o, MovedView (view, o, 0), hud, nextFrame++);
                std::ofstream csv (o.out + "/" + view.name + "_" + style + "_motion.csv");
                csv << "frame,flicker,flickerPixels,error\n";
                std::vector<uint8_t> previous;
                double meanSum = 0.0, errorSum = 0.0;
                size_t pixelSum = 0, worstPixels = 0;
                uint32_t scored = 0;
                for (uint32_t k = 0; k < o.motion && k < references.size (); ++k) {
                    RenderFrame (scene, context, targets, o, MovedView (view, o, k), hud, nextFrame++);
                    std::vector<uint8_t> current = ReadBack (context, targets, o.width, o.height);
                    if (k == o.motion / 2) {
                        stbi_write_png ((o.out + "/" + view.name + "_" + style + "_moving.png").c_str (), int (o.width),
                                        int (o.height), 4, current.data (), int (o.width) * 4);
                        stbi_write_png ((o.out + "/" + view.name + "_" + style + "_moving_ref.png").c_str (),
                                        int (o.width), int (o.height), 4, references[k].data (), int (o.width) * 4);
                    }
                    if (k >= 1) {
                        const Flicker f =
                            MotionFlicker (previous, references[k - 1], current, references[k], kThreshold);
                        // Where it flickers: the change in error at the middle of the path, x4, grey.
                        if (k == o.motion / 2) {
                            std::vector<uint8_t> heat (size_t (o.width) * o.height);
                            for (size_t p = 0; p < heat.size (); ++p) {
                                int worst = 0;
                                for (size_t c = 0; c < 3; ++c)
                                    worst = std::max (
                                        worst,
                                        std::abs ((int (current[p * 4 + c]) - int (references[k][p * 4 + c])) -
                                                  (int (previous[p * 4 + c]) - int (references[k - 1][p * 4 + c]))));
                                heat[p] = uint8_t (std::min (255, worst * 4));
                            }
                            stbi_write_png ((o.out + "/" + view.name + "_" + style + "_flicker.png").c_str (),
                                            int (o.width), int (o.height), 1, heat.data (), int (o.width));
                        }
                        csv << k << "," << f.mean << "," << f.pixels << "," << f.error << "\n";
                        meanSum += f.mean;
                        errorSum += f.error;
                        pixelSum += f.pixels;
                        worstPixels = std::max (worstPixels, f.pixels);
                        ++scored;
                    }
                    previous = std::move (current);
                }
                const double mean = scored > 0 ? meanSum / scored : 0.0;
                const double pixels = scored > 0 ? double (pixelSum) / scored : 0.0;
                const double error = scored > 0 ? errorSum / scored : 0.0;
                av::ArchVizLog ("bench: motion " + view.name + "_" + style + ": " + std::to_string (o.motion) +
                                " frames " + (o.motionOrbit ? "orbit" : "dolly") + ", flicker " +
                                std::to_string (mean) + " levels, " + std::to_string (pixels) +
                                " px > 24 a frame (worst " + std::to_string (worstPixels) + "), error vs " +
                                std::to_string (kMotionSupersample) + "x supersampled " + std::to_string (error) +
                                " levels");
                temporalJson += ", \"motion\": { \"frames\": " + std::to_string (o.motion) +
                                ", \"flicker\": " + std::to_string (mean) +
                                ", \"flickerPixels\": " + std::to_string (pixels) +
                                ", \"error\": " + std::to_string (error) + " }";
            }
            const av::RayTracedStats s = scene.RayTracedStatus ();
            av::ArchVizLog ("bench: " + file + (saved ? " saved" : " NOT saved") + " in " + std::to_string (seconds) +
                            " s; ray tracing " + av::RayTracedStateName (s.state) + " " + std::to_string (s.samples) +
                            "/" + std::to_string (s.maxSamples) + " samples, reflections " +
                            (s.reflectionsTraced ? "traced" : "screen-space") + ", sky light " +
                            (s.skyLightTraced ? "traced" : "AO") + ", textured materials " +
                            std::to_string (s.texturedMaterials) + ", " + std::to_string (framesRun) + " frames, " +
                            std::to_string (scene.Stats ().drawCalls) + " draws, " +
                            std::to_string (scene.Stats ().meshCommits) + " commits" +
                            (s.msPerSample > 0.0 ? ", " + std::to_string (s.samplesPerFrame) + " samples/frame at " +
                                                       std::to_string (s.msPerSample) + " ms/sample"
                                                 : std::string ()) +
                            (s.textureOverflow > 0 ? " (" + std::to_string (s.textureOverflow) + " over the limit)"
                                                   : std::string ()) +
                            (s.reason.empty () ? std::string () : " (" + s.reason + ")"));
            report << (firstRender ? "" : ",\n") << "    { \"view\": \"" << view.name << "\", \"style\": \"" << style
                   << "\", \"file\": \"" << file << "\", \"seconds\": " << seconds << ", \"rtState\": \""
                   << av::RayTracedStateName (s.state) << "\", \"samples\": " << s.samples
                   << ", \"reflectionsTraced\": " << (s.reflectionsTraced ? "true" : "false")
                   << ", \"skyLightTraced\": " << (s.skyLightTraced ? "true" : "false") << temporalJson << " }";
            firstRender = false;
        }
    }
    report << "\n  ]\n}\n";
    scene.Shutdown ();
    return 0;
}
