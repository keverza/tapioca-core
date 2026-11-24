#pragma once

#include "ArchViz/DiligentHudCaptureFrame.hpp"
#include "ArchViz/LayoutReportPage.hpp"
#include <RenderDevice.h>
#include <Texture.h>
#include <RefCntAutoPtr.hpp>

namespace geomsrv::archviz::viewerhud {
struct ReportPaperPreview {
    LayoutReportPage page;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    float composedAspect = -1;
};

inline void UpdateReportPaper (Diligent::IRenderDevice* device, CaptureFrame& f, bool south)
{
    if (!f.paperCache)
        f.paperCache = std::make_shared<ReportPaperPreview> ();
    auto& cache = *f.paperCache;
    auto& p = cache.page;
    if (f.paperDirty) {
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
                t.metric = "Awaiting 3D view";
                p.panels.push_back (std::move (t));
            }
        p.site = "Render preview to populate measured views and study legend";
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
    desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
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
