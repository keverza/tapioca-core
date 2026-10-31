// The live viewer's screenshot: see ViewerScreenshot.hpp for why it borrows the
// frame loop rather than the headless capture.
//
// ⚠️ ITS OWN TRANSLATION UNIT because DiligentViewport.cpp is a frozen-size
// exception: what stays there is the ORDER (which target the frame draws into,
// where the legend replaces the HUD, which frames are not presented); what a
// screenshot does with the pixels lives here, like DiligentCapture.cpp.
//
// RENDER THREAD. It never touches ACAPI or DG, which is what lets it write the
// file directly.

#include "ArchViz/ViewerScreenshot.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <EngineFactoryD3D11.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace geomsrv::archviz {

namespace {

// UTF-8 for the log and the HUD. ⚠️ NOT path::u8string, which is
// std::u8string under C++20 and concatenates with nothing here.
std::string Utf8 (const std::filesystem::path& path)
{
    const std::u8string text = path.u8string ();
    return std::string (text.begin (), text.end ());
}

// %LOCALAPPDATA%\Tapioca\Export, created if missing: a screenshot is a
// deliverable, not a diagnostic, so it does not go in the logs.
bool ExportDirectory (std::filesystem::path& out, std::string& error)
{
    const wchar_t* root = _wgetenv (L"LOCALAPPDATA");
    if (root == nullptr || *root == 0) {
        error = "LOCALAPPDATA is unavailable";
        return false;
    }
    out = std::filesystem::path (root) / L"Tapioca" / L"Export";
    std::error_code code;
    std::filesystem::create_directories (out, code);
    if (code) {
        error = "could not create " + Utf8 (out) + ": " + code.message ();
        return false;
    }
    return true;
}

// "sunstudy-20261031-142205-2x.png". The timestamp to the second keeps a
// sequence of shots in the order they were taken.
std::string ShotName (float scale)
{
    const std::time_t now = std::chrono::system_clock::to_time_t (std::chrono::system_clock::now ());
    std::tm local = {};
    localtime_s (&local, &now);
    char stamp[32] = {};
    std::strftime (stamp, sizeof (stamp), "%Y%m%d-%H%M%S", &local);
    char name[64] = {};
    std::snprintf (name, sizeof (name), "sunstudy-%s-%gx.png", stamp, double (scale));
    return name;
}

void Fail (HudState& state, const std::string& reason)
{
    state.sunShotFailed = true;
    state.sunShotBusy = false;
    state.sunShotResult = reason;
    ArchVizLog ("viewer screenshot: not saved -- " + reason);
}

} // namespace

ViewerScreenshot::ViewerScreenshot () = default;
ViewerScreenshot::~ViewerScreenshot () = default;

bool ViewerScreenshot::Active () const
{
    return target_ != nullptr;
}

DiligentViewportTarget& ViewerScreenshot::TargetFor (DiligentViewportTarget& surface, Diligent::IRenderDevice* device,
                                                     Diligent::IDeviceContext* context, DiligentScene& scene,
                                                     HudState& state)
{
    if (target_ != nullptr)
        return *target_;
    if (!state.sunShotRequested)
        return surface;
    state.sunShotRequested = false;
    if (surface.Mode () != SurfaceMode::PaletteChild ||
        surface.ColorFormat () != uint32_t (Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB)) {
        Fail (state, "only the palette viewer can take one, not the overlay");
        return surface;
    }
    const int preset = std::clamp (state.sunShotPreset, 0, kViewerScreenshotScaleCount - 1);
    scale_ = kViewerScreenshotScales[preset];
    const uint32_t width = ViewerScreenshotSize (surface.Width (), scale_);
    const uint32_t height = ViewerScreenshotSize (surface.Height (), scale_);
    auto target = std::make_unique<DiligentViewportTarget> ();
    std::string error;
    if (width == 0 || height == 0 ||
        !target->Create (device, context, Diligent::GetEngineFactoryD3D11 (), SurfaceMode::Offscreen, nullptr, width,
                         height, error)) {
        target->Destroy (context);
        Fail (state, error.empty () ? "the viewer has no size yet" : error);
        return surface;
    }
    target_ = std::move (target);
    device_ = device;
    frames_ = 0;
    state.sunShotBusy = true;
    // The window's history is the wrong size and, at worst, the wrong pixels.
    scene.ResetTemporalAntiAliasingHistory ();
    ArchVizLog ("viewer screenshot: rendering " + std::to_string (width) + "x" + std::to_string (height));
    return *target_;
}

// The legend as its own white strip, the image's width, read back for stacking
// under the view. ⚠️ ITS OWN TARGET, NOT ROWS RESERVED IN THE VIEW'S: the
// scene draws into the whole of whatever it is given, so a taller target would
// stretch the camera instead of leaving room.
static bool RenderLegendStrip (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, DiligentHud& hud,
                               DiligentScene& scene, const HudState& state, uint32_t width, uint32_t height,
                               float scale, std::vector<uint8_t>& rgba, std::string& error)
{
    DiligentViewportTarget strip;
    if (!strip.Create (device, context, Diligent::GetEngineFactoryD3D11 (), SurfaceMode::Offscreen, nullptr, width,
                       height, error))
        return false;
    Diligent::ITextureView* rtv = nullptr;
    Diligent::ITextureView* dsv = nullptr;
    bool ok = strip.BeginFrame (rtv, dsv);
    if (ok) {
        constexpr float kWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->ClearRenderTarget (rtv, kWhite, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        hud.DrawCaptureLegend (context, width, height, scale, scene.Stats (), state);
        context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
        ok = strip.CaptureRgba (context, rgba, error);
    }
    else
        error = "the legend strip returned no render target";
    strip.Destroy (context);
    return ok;
}

bool ViewerScreenshot::Finish (Diligent::IDeviceContext* context, DiligentScene& scene, DiligentHud& hud,
                               HudState& state)
{
    if (target_ == nullptr)
        return false;
    if (++frames_ < kViewerScreenshotSettleFrames)
        return true;

    const uint32_t width = target_->Width ();
    const uint32_t viewHeight = target_->Height ();
    const uint32_t stripHeight = hud.CaptureLegendHeight (scale_);
    std::vector<uint8_t> image;
    std::vector<uint8_t> strip;
    std::string png;
    std::string error;
    std::filesystem::path directory;
    // The view on top, the legend strip under it, one PNG.
    bool ok = target_->CaptureRgba (context, image, error);
    if (ok && stripHeight > 0) {
        ok = RenderLegendStrip (device_, context, hud, scene, state, width, stripHeight, scale_, strip, error);
        image.insert (image.end (), strip.begin (), strip.end ());
    }
    ok = ok && EncodeRgbaPng (image, width, viewHeight + (strip.empty () ? 0u : stripHeight), png, error) &&
         ExportDirectory (directory, error);
    if (!ok)
        Fail (state, error);
    else {
        const std::filesystem::path path = directory / ShotName (scale_);
        std::ofstream file (path, std::ios::binary);
        file.write (png.data (), static_cast<std::streamsize> (png.size ()));
        if (!file)
            Fail (state, "could not write " + Utf8 (path));
        else {
            state.sunShotFailed = false;
            state.sunShotBusy = false;
            state.sunShotResult = Utf8 (path);
            ArchVizLog ("viewer screenshot: saved " + state.sunShotResult + " (" + std::to_string (png.size ()) +
                        " bytes)");
        }
    }

    Destroy (context);
    // Back to the window: its history is now the screenshot's size.
    scene.ResetTemporalAntiAliasingHistory ();
    return true;
}

void ViewerScreenshot::Destroy (Diligent::IDeviceContext* context)
{
    if (target_ == nullptr)
        return;
    target_->Destroy (context);
    target_.reset ();
}

} // namespace geomsrv::archviz
