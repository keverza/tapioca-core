// DiagramBench -- the viewer's exploded massing diagram outside Archicad (#201).
//
// Replays a diagram capture (the viewer's Massing tab -> Save capture (JSON)) through the same
// steps the live viewer takes -- ReplayCapture, Prepare, and DiligentScene::DrawDiagram with a
// DiagramView -- and writes one PNG per view, gap and colouring. A spacing, a per-building gap
// or a style can be checked and compared without starting Archicad.
//
//   TapiocaDiagramBench --capture diagram-capture.json --out dir [--size 1280x800]
//       [--gaps 0,3,8] [--building building:A=12 ...] [--colour function|white|both]
//       [--views iso,front,side,top]
//
// ⚠️ THE VIEWER'S DRAW, NOT A COPY OF IT. Everything after the capture is the add-on's own code;
// only the camera (framed on the spaced plan's bounds) and the clear colour are the bench's.

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiagramReplay.hpp"
#include "ArchViz/DiagramView.hpp"
#include "ArchViz/DiligentDeviceFactory.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <DeviceContext.h>
#include <RenderDevice.h>
#include <Texture.h>
#include <TextureView.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace av = geomsrv::archviz;
namespace dg = geomsrv::archviz::diagram;

namespace geomsrv::archviz {
// The add-on's log writes through Archicad; here it is the console.
void ArchVizLog (const std::string& line)
{
    std::fprintf (stderr, "%s\n", line.c_str ());
}
} // namespace geomsrv::archviz

namespace {
struct Options {
    std::string capture, out;
    uint32_t width = 1280, height = 800;
    std::vector<double> gaps { 0.0, 3.0, 8.0 };
    std::map<std::string, double> buildingGaps;
    std::vector<dg::Colouring> colourings { dg::Colouring::Function };
    std::vector<std::string> views { "iso", "front", "side" };
};

std::vector<std::string> Split (const std::string& text, char by)
{
    std::vector<std::string> parts;
    std::stringstream in (text);
    for (std::string part; std::getline (in, part, by);)
        if (!part.empty ())
            parts.push_back (part);
    return parts;
}

bool ParseArgs (int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&] () -> std::string { return i + 1 < argc ? argv[++i] : std::string (); };
        if (a == "--capture")
            o.capture = next ();
        else if (a == "--out")
            o.out = next ();
        else if (a == "--size") {
            const auto parts = Split (next (), 'x');
            if (parts.size () != 2)
                return false;
            o.width = uint32_t (std::stoul (parts[0]));
            o.height = uint32_t (std::stoul (parts[1]));
        }
        else if (a == "--gaps") {
            o.gaps.clear ();
            for (const auto& part : Split (next (), ','))
                o.gaps.push_back (std::stod (part));
        }
        else if (a == "--building") {
            const std::string spec = next ();
            const size_t eq = spec.rfind ('=');
            if (eq == std::string::npos)
                return false;
            o.buildingGaps[spec.substr (0, eq)] = std::stod (spec.substr (eq + 1));
        }
        else if (a == "--colour") {
            const std::string c = next ();
            o.colourings.clear ();
            if (c == "function" || c == "both")
                o.colourings.push_back (dg::Colouring::Function);
            if (c == "white" || c == "both")
                o.colourings.push_back (dg::Colouring::White);
            if (o.colourings.empty ())
                return false;
        }
        else if (a == "--views")
            o.views = Split (next (), ',');
        else
            return false;
    }
    return !o.capture.empty () && !o.out.empty () && !o.gaps.empty () && o.width > 0 && o.height > 0;
}

struct Targets {
    Diligent::RefCntAutoPtr<Diligent::ITexture> color, depth, staging;
};

bool CreateTargets (Diligent::IRenderDevice* device, uint32_t w, uint32_t h, Targets& t)
{
    Diligent::TextureDesc d;
    d.Type = Diligent::RESOURCE_DIM_TEX_2D;
    d.Width = w;
    d.Height = h;
    d.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    d.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
    d.Name = "diagram bench colour";
    device->CreateTexture (d, nullptr, &t.color);
    d.Format = Diligent::TEX_FORMAT_D32_FLOAT;
    d.BindFlags = Diligent::BIND_DEPTH_STENCIL;
    d.Name = "diagram bench depth";
    device->CreateTexture (d, nullptr, &t.depth);
    d.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    d.BindFlags = Diligent::BIND_NONE;
    d.Usage = Diligent::USAGE_STAGING;
    d.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    d.Name = "diagram bench readback";
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

// A direction to look from, toward the bounds' centre: Z up, the scene's own axes.
bool ViewDirection (const std::string& name, float dir[3])
{
    const auto set = [&] (float x, float y, float z) {
        const float n = std::sqrt (x * x + y * y + z * z);
        dir[0] = x / n;
        dir[1] = y / n;
        dir[2] = z / n;
    };
    if (name == "iso")
        set (1.0f, -1.2f, 0.9f);
    else if (name == "front")
        set (0.0f, -1.0f, 0.12f);
    else if (name == "side")
        set (1.0f, 0.0f, 0.12f);
    else if (name == "top")
        set (0.001f, -0.001f, 1.0f);
    else
        return false;
    return true;
}
} // namespace

int main (int argc, char** argv)
{
    Options o;
    if (!ParseArgs (argc, argv, o)) {
        std::fprintf (stderr, "usage: TapiocaDiagramBench --capture file.json --out dir [--size WxH] [--gaps 0,3,8] "
                              "[--building building:A=12 ...] [--colour function|white|both] "
                              "[--views iso,front,side,top]\n");
        return 2;
    }
    std::filesystem::create_directories (o.out);
    std::ifstream in (o.capture, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf ();
    dg::DiagramInputSnapshot snapshot;
    std::string error;
    if (!dg::ReplayCapture (text.str (), snapshot, error)) {
        av::ArchVizLog ("diagram bench: " + o.capture + ": " + error);
        return 1;
    }
    auto prepared = dg::Prepare (snapshot, {}, dg::DefaultRequest (), 1);
    for (const auto& note : prepared.notes)
        av::ArchVizLog ("diagram bench: note: " + note);
    if (!prepared.publication) {
        av::ArchVizLog ("diagram bench: the capture was refused");
        return 1;
    }
    const auto& publication = *prepared.publication;
    av::ArchVizLog ("diagram bench: " + std::to_string (publication.buildings) + " building(s), " +
                    std::to_string (publication.storeys) + " storey(s), " +
                    std::to_string (publication.geometry.vertices.size ()) + " vertices, built in " +
                    std::to_string (publication.buildMs) + " ms");
    for (const auto& node : publication.plan.nodes)
        if (node.id.level == dg::Level::Building)
            av::ArchVizLog ("diagram bench: building key " + node.id.key + " (--building " + node.id.key + "=<m>)");

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    av::RenderBackendChoice choice;
    if (!av::CreateViewportDevice (av::RenderBackendPreference::Auto, av::ViewportBackendConstraints (false), device,
                                   context, choice, error)) {
        av::ArchVizLog ("diagram bench: no device: " + error);
        return 1;
    }
    Targets targets;
    if (!CreateTargets (device, o.width, o.height, targets)) {
        av::ArchVizLog ("diagram bench: could not create the render targets");
        return 1;
    }
    av::DiligentScene scene;
    if (!scene.Init (device, uint32_t (Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB),
                     uint32_t (Diligent::TEX_FORMAT_D32_FLOAT), error)) {
        av::ArchVizLog ("diagram bench: scene init failed: " + error);
        return 1;
    }

    int written = 0;
    for (const double gap : o.gaps)
        for (const auto colouring : o.colourings) {
            dg::DiagramView view;
            view.publication = prepared.publication;
            view.spacing.storeyGap = gap;
            view.spacing.buildingGap = o.buildingGaps;
            view.colouring = colouring;
            view.animate = false; // the asked spacing at once: one picture per setting
            dg::Vec3 lo {}, hi {};
            dg::FramingBounds (dg::Spaced (publication.plan, view.spacing), lo, hi);
            const float centre[3] = { float (0.5 * (lo[0] + hi[0])), float (0.5 * (lo[1] + hi[1])),
                                      float (0.5 * (lo[2] + hi[2])) };
            const float radius =
                float (0.5 * std::sqrt ((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                        (hi[2] - lo[2]) * (hi[2] - lo[2]))) +
                1.0f;
            for (const auto& name : o.views) {
                float dir[3];
                if (!ViewDirection (name, dir)) {
                    av::ArchVizLog ("diagram bench: unknown view '" + name + "' (iso, front, side, top)");
                    continue;
                }
                const float fov = 35.0f;
                const float distance = radius / std::sin (0.5f * fov * 3.14159265f / 180.0f) * 1.05f;
                const float eye[3] = { centre[0] + dir[0] * distance, centre[1] + dir[1] * distance,
                                       centre[2] + dir[2] * distance };
                const float up[3] = { 0.0f, 0.0f, 1.0f };
                float view16[16], proj[16], viewProj[16];
                av::LookAtRH (view16, eye, centre, up);
                av::PerspectiveRH (proj, fov, float (o.width) / float (o.height),
                                   (std::max) (0.05f, distance - radius * 2), distance + radius * 2);
                av::Multiply (viewProj, view16, proj);

                Diligent::ITextureView* rtv = targets.color->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
                Diligent::ITextureView* dsv = targets.depth->GetDefaultView (Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
                const float clear[4] = { 0.82f, 0.82f, 0.83f, 1.0f }; // the viewer's grey background
                context->SetRenderTargets (1, &rtv, dsv, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                context->SetViewports (1, nullptr, o.width, o.height);
                context->ClearRenderTarget (rtv, clear, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                context->ClearDepthStencil (dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                const auto stats =
                    scene.DrawDiagram (context, viewProj, uint32_t (Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB),
                                       uint32_t (Diligent::TEX_FORMAT_D32_FLOAT), &view);
                char file[160];
                std::snprintf (file, sizeof (file), "%s_gap%.1f_%s.png", name.c_str (), gap,
                               colouring == dg::Colouring::White ? "white" : "function");
                const std::string path = (std::filesystem::path (o.out) / file).string ();
                if (!SavePng (context, targets, o.width, o.height, path)) {
                    av::ArchVizLog ("diagram bench: could not write " + path);
                    return 1;
                }
                ++written;
                av::ArchVizLog ("diagram bench: " + path + " (" + std::to_string (stats.draws) + " draws, " +
                                std::to_string (stats.uploads) + " upload(s))");
            }
        }
    av::ArchVizLog ("diagram bench: " + std::to_string (written) + " picture(s) in " + o.out);
    return written > 0 ? 0 : 1;
}
