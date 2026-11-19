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
        else if (a == "--frames")
            o.frames = uint32_t (std::stoul (next ()));
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
    hud.technicalEdges = false;
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
        hud.temporalAntiAliasing = false;
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
        // "ssr" is the fallback every non-DXR machine sees; "traced" is Rendered on DXR.
        hud.rayTracedReflections = style == "traced";
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

bool SavePng (Diligent::IDeviceContext* context, Targets& t, uint32_t w, uint32_t h, const std::string& path)
{
    Diligent::CopyTextureAttribs copy (t.color, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, t.staging,
                                       Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->CopyTexture (copy);
    context->WaitForIdle ();
    Diligent::MappedTextureSubresource mapped;
    context->MapTextureSubresource (t.staging, 0, 0, Diligent::MAP_READ, Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr,
                                    mapped);
    if (mapped.pData == nullptr)
        return false;
    std::vector<uint8_t> rows (size_t (w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy (&rows[size_t (y) * w * 4], static_cast<const uint8_t*> (mapped.pData) + size_t (y) * mapped.Stride,
                     size_t (w) * 4);
    context->UnmapTextureSubresource (t.staging, 0, 0);
    return stbi_write_png (path.c_str (), int (w), int (h), 4, rows.data (), int (w) * 4) != 0;
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
    const bool taa = hud.temporalAntiAliasing && hud.renderQuality == int (av::RenderQuality::Realistic);
    scene.SetTemporalAntiAliasing (taa, hud.taaStability);
    scene.PrepareTemporalAntiAliasingFrame (context, frame, stableProj, proj, jitter);
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
                    std::to_string (obj.elements.size ()) + " materials in use, bounds " +
                    std::to_string (obj.boundsMin[0]) + "," + std::to_string (obj.boundsMin[1]) + "," +
                    std::to_string (obj.boundsMin[2]) + " .. " + std::to_string (obj.boundsMax[0]) + "," +
                    std::to_string (obj.boundsMax[1]) + "," + std::to_string (obj.boundsMax[2]) + ", loaded in " +
                    std::to_string (loadSeconds) + " s");
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
            for (uint32_t f = 0; f < frames; ++f) {
                RenderFrame (scene, context, targets, o, view, hud, f);
                if (reference && f % 16 == 15) {
                    const av::RayTracedStats s = scene.RayTracedStatus ();
                    if (s.state == av::RayTracedState::Converged || s.state == av::RayTracedState::Unavailable)
                        break;
                }
            }
            // The caps are cut on a worker: keep rendering until the current plane's build has landed,
            // so the picture is never the back faces standing in for caps that were still coming.
            for (int extra = 0; o.section && extra < 300; ++extra) {
                const av::SectionCapCounts caps = scene.Stats ().sectionCaps;
                if (!caps.building && extra >= 2) {
                    av::ArchVizLog ("bench: section caps: " + std::to_string (caps.elementsCut) + " elements cut, " +
                                    std::to_string (caps.closedLoops) + " closed, " + std::to_string (caps.openChains) +
                                    " open, " + std::to_string (caps.fillFailures) + " refused");
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
                RenderFrame (scene, context, targets, o, view, hud, frames + uint32_t (extra));
            }
            context->WaitForIdle ();
            const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
            const std::string file = view.name + "_" + style + ".png";
            const bool saved = SavePng (context, targets, o.width, o.height, o.out + "/" + file);
            const av::RayTracedStats s = scene.RayTracedStatus ();
            av::ArchVizLog ("bench: " + file + (saved ? " saved" : " NOT saved") + " in " + std::to_string (seconds) +
                            " s; ray tracing " + av::RayTracedStateName (s.state) + " " + std::to_string (s.samples) +
                            "/" + std::to_string (s.maxSamples) + " samples, reflections " +
                            (s.reflectionsTraced ? "traced" : "screen-space") + ", sky light " +
                            (s.skyLightTraced ? "traced" : "AO") +
                            (s.reason.empty () ? std::string () : " (" + s.reason + ")"));
            report << (firstRender ? "" : ",\n") << "    { \"view\": \"" << view.name << "\", \"style\": \"" << style
                   << "\", \"file\": \"" << file << "\", \"seconds\": " << seconds << ", \"rtState\": \""
                   << av::RayTracedStateName (s.state) << "\", \"samples\": " << s.samples
                   << ", \"reflectionsTraced\": " << (s.reflectionsTraced ? "true" : "false")
                   << ", \"skyLightTraced\": " << (s.skyLightTraced ? "true" : "false") << " }";
            firstRender = false;
        }
    }
    report << "\n  ]\n}\n";
    scene.Shutdown ();
    return 0;
}
