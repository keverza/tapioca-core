#pragma once

#include "ArchViz/DiligentHudCaptureFrame.hpp"
#include "ArchViz/LayoutReportPage.hpp"
#include <RenderDevice.h>
#include <DeviceContext.h>
#include <Texture.h>
#include <RefCntAutoPtr.hpp>
#include <functional>
#include <array>
#include <chrono>

namespace geomsrv::archviz::viewerhud {
struct ReportPaperPreview {
    LayoutReportPage page;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::RefCntAutoPtr<Diligent::ITexture> live;
    std::vector<Diligent::RefCntAutoPtr<Diligent::ITexture>> clockTextures;
    std::string study;
    uint64_t version = 0;
    float composedAspect = -1;
    std::array<float, 32> camera {};
    size_t tilesToRefresh = 0;
    std::chrono::steady_clock::time_point navigationUntil {};
};

// Copy the settled scene before ImGui paints the paper over the swap-chain buffer.
// ImGui decodes sampled RGB itself, so its texture must return encoded bytes.
inline void CopyPaperScene (CaptureFrame& f, Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                            Diligent::ITextureView* colour, Diligent::ITextureView* depth)
{
    f.liveImage = 0;
    if (!f.active || !f.builder || !f.paperCache ||
        (!f.paperCache->page.pending && !(f.measuredDraft && !f.clockImages.empty ())) || !colour)
        return;
    const bool byClock = f.measuredDraft || (f.reportType == int (ReportTemplate::Analysis) && !f.panelSteps.empty ());
    if (byClock &&
        (f.liveColumn >= f.clockImages.size () || f.liveColumn >= f.paperCache->clockTextures.size () ||
         (f.measuredDraft ? f.liveStep < 0 : f.liveColumn >= f.panelSteps.size () || f.panelSteps[f.liveColumn] < 0)))
        return;
    auto& live = byClock ? f.paperCache->clockTextures[f.liveColumn] : f.paperCache->live;
    const auto& src = colour->GetTexture ()->GetDesc ();
    if (!live || live->GetDesc ().Width != src.Width || live->GetDesc ().Height != src.Height) {
        Diligent::TextureDesc desc;
        desc.Name = "Layout paper live view";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = src.Width;
        desc.Height = src.Height;
        desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        if (byClock)
            f.clockImages[f.liveColumn] = 0;
        live.Release ();
        device->CreateTexture (desc, nullptr, &live);
    }
    if (!live)
        return;
    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = colour->GetTexture ();
    copy.pDstTexture = live;
    copy.SrcTextureTransitionMode = copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copy);
    context->SetRenderTargets (1, &colour, depth, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    f.liveImage = reinterpret_cast<uintptr_t> (live->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    if (byClock)
        f.clockImages[f.liveColumn] = f.liveImage;
}

inline void UpdateReportPaper (Diligent::IRenderDevice* device, CaptureFrame& f, bool south,
                               const std::function<void (LayoutReportPage&)>& legend = {})
{
    if (!f.paperCache)
        f.paperCache = std::make_shared<ReportPaperPreview> ();
    auto& cache = *f.paperCache;
    auto& p = cache.page;
    if (f.paperDirty) {
        f.clockImages.assign (f.panelSteps.size () * (f.measuredDraft ? 3 : 1), 0);
        cache.clockTextures.clear ();
        cache.clockTextures.resize (f.clockImages.size ());
        cache.tilesToRefresh = f.clockImages.size ();
        f.liveColumn = 0;
        p = {};
        p.type = ReportTemplate (f.reportType);
        p.format = f.pageFormat;
        p.pending = true;
        p.clocks = f.clocks;
        const size_t cols = f.reportType == 0 ? p.clocks.size () : 1;
        for (int row = 0; row < 3; ++row)
            for (size_t c = 0; c < cols; ++c) {
                ReportPanel t;
                t.date = !row ? "20 March" : (row == 1) != south ? "21 June" : "21 December";
                t.season = !row ? "Equinox" : row == 1 ? "Summer solstice" : "Winter solstice";
                t.metric =
                    f.reportType == int (ReportTemplate::Analysis) && c < f.panelSteps.size () && f.panelSteps[c] < 0
                        ? "Not measured"
                        : "Current study";
                p.panels.push_back (std::move (t));
            }
        if (legend)
            legend (p);
        cache.composedAspect = -1;
        f.paperDirty = false;
    }
    if (cache.composedAspect == f.tileAspect)
        return;
    p.tileAspect = f.tileAspect;
    std::vector<uint8_t> pixels;
    f.paperImage = 0;
    if (!ComposeReportPage (p, pixels, &f.paperTiles))
        return;
    const auto size = ReportPageFormat (p.format);
    Diligent::TextureDesc desc;
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = size.width;
    desc.Height = size.height;
    desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    Diligent::TextureSubResData sub;
    sub.pData = pixels.data ();
    sub.Stride = size.width * 4;
    Diligent::TextureData data;
    data.pSubResources = &sub;
    data.NumSubresources = 1;
    cache.texture.Release ();
    device->CreateTexture (desc, &data, &cache.texture);
    if (cache.texture)
        f.paperImage =
            reinterpret_cast<uintptr_t> (cache.texture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    cache.composedAspect = f.tileAspect;
}
} // namespace geomsrv::archviz::viewerhud
