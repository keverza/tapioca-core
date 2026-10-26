// OVERLAY-INVARIANTS.md: shared prepared arrays/pipelines only, not camera
// discovery or injection state. This runs exclusively on the viewer render thread.
#include "ArchViz/ViewerEditorDraw.hpp"
#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/GuestShaderSources.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <Texture.h>
#include <TextureView.h>

namespace geomsrv::archviz {
namespace gpu = dxgi::guestgpu;
namespace {
bool Upload (Diligent::IRenderDevice* device, const gpu::Pipelines& pipelines, const overlayscene::Scene& scene,
             std::vector<gpu::Page>& pages, gpu::Content& content, std::string& error)
{
    gpu::Arrays arrays;
    arrays.fills = scene.fills.data ();
    arrays.fillCount = scene.fills.size ();
    arrays.fillStride = sizeof (overlayscene::SceneFillVertex);
    arrays.lines = scene.lines.data ();
    arrays.lineCount = scene.lines.size ();
    arrays.lineStride = sizeof (overlayscene::SceneLine);
    arrays.glyphs = scene.glyphs.data ();
    arrays.glyphCount = scene.glyphs.size ();
    arrays.glyphStride = sizeof (overlayscene::SceneGlyph);
    arrays.dashes = &scene.dashes;
    arrays.solid = scene.solid;
    return gpu::Upload (device, pipelines, pages, arrays, scene.fillDraws, scene.glyphDraws, scene.pages, content,
                        error);
}
// ⚠️ THE OVERLAY'S COLOURS ARE ALREADY sRGB-ENCODED BYTES. The injected path writes
// them as-is into Archicad's UNORM back buffer; the viewer's swap-chain RTV is
// _SRGB, which encodes them a second time and lifts every panel toward white.
// Diligent's flip chain keeps the TEXTURE UNORM and only the view _SRGB
// (SwapChainD3DBase), so the editor draws through a UNORM view of the same
// texture and stores exactly the bytes the overlay stores.
Diligent::TEXTURE_FORMAT StoredEncoding (Diligent::TEXTURE_FORMAT format)
{
    switch (format) {
        case Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB:
            return Diligent::TEX_FORMAT_RGBA8_UNORM;
        case Diligent::TEX_FORMAT_BGRA8_UNORM_SRGB:
            return Diligent::TEX_FORMAT_BGRA8_UNORM;
        case Diligent::TEX_FORMAT_BGRX8_UNORM_SRGB:
            return Diligent::TEX_FORMAT_BGRX8_UNORM;
        default:
            return format;
    }
}
// Per frame, never cached: a retained view of a swap-chain buffer makes
// ResizeBuffers fail (OVERLAY-INVARIANTS.md §11). Null keeps the caller's view.
Diligent::RefCntAutoPtr<Diligent::ITextureView> StoredEncodingView (Diligent::ITextureView* target)
{
    Diligent::RefCntAutoPtr<Diligent::ITextureView> view;
    Diligent::ITexture* texture = target != nullptr ? target->GetTexture () : nullptr;
    const auto format = target != nullptr ? target->GetDesc ().Format : Diligent::TEX_FORMAT_UNKNOWN;
    if (texture == nullptr || StoredEncoding (format) == format)
        return view;
    Diligent::TextureViewDesc desc = target->GetDesc ();
    desc.Name = "Viewer editor UNORM target";
    desc.Format = StoredEncoding (format);
    texture->CreateView (desc, &view);
    return view;
}
bool Matrix (Diligent::IDeviceContext* context, Diligent::IBuffer* buffer, const std::array<float, 16>& matrix)
{
    void* data = nullptr;
    context->MapBuffer (buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, data);
    if (!data)
        return false;
    std::memcpy (data, matrix.data (), sizeof (float) * 16);
    context->UnmapBuffer (buffer, Diligent::MAP_WRITE);
    return true;
}
} // namespace

void ViewerEditorDraw::Update (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                               Diligent::ITextureView* target, Diligent::ITextureView* depth, const float* view,
                               const float* projection, uint32_t width, uint32_t height)
{
    if (!vieweredit::Running ()) {
        Clear ();
        return;
    }
    const auto generation = vieweredit::Generation ();
    if (generation_ != generation) {
        Clear ();
        generation_ = generation;
    }
    vieweredit::Frame frame;
    std::copy_n (view, 16, frame.view.begin ());
    std::copy_n (projection, 16, frame.projection.begin ());
    frame.width = width;
    frame.height = height;
    vieweredit::PublishFrame (frame);
    if (!vieweredit::Active ())
        return;
    const auto stored = StoredEncodingView (target);
    Diligent::ITextureView* bound = stored ? stored.RawPtr () : target;
    context->SetRenderTargets (1, &bound, depth, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::Viewport viewport;
    viewport.Width = float (width);
    viewport.Height = float (height);
    context->SetViewports (1, &viewport, width, height);
    std::string error;
    const bool drawn = Draw (device, context, bound->GetDesc ().Format,
                             depth ? depth->GetDesc ().Format : Diligent::TEX_FORMAT_UNKNOWN, frame, error);
    // The viewer's own layers follow on its _SRGB view, and the context must not
    // keep the per-frame view alive into Present or a resize.
    context->SetRenderTargets (1, &target, depth, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (!drawn)
        throw std::runtime_error ("Viewer editor: " + error);
}

bool ViewerEditorDraw::Draw (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                             Diligent::TEXTURE_FORMAT colour, Diligent::TEXTURE_FORMAT depth,
                             const vieweredit::Frame& frame, std::string& error)
{
    if (!pipelines_.ready) {
        const auto body = dxgi::guestshaders::SceneBody ();
        std::vector<char> source (body.size () + 4096);
        // Ordinary row-vector view/projection, the viewer's. No relative-camera
        // interpretation or injected snapshot ever enters this path.
        if (!dxgi::camerashader::Compose (0, body.c_str (), source.data (), source.size ()) ||
            !gpu::Build (device, gpu::Kind::Scene, source.data (), pipelines_, error, colour, depth))
            return false;
        Diligent::BufferDesc desc;
        desc.Name = "Viewer editor camera";
        desc.Size = 64;
        desc.Usage = Diligent::USAGE_DYNAMIC;
        desc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
        desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
        device->CreateBuffer (desc, nullptr, &view_);
        device->CreateBuffer (desc, nullptr, &projection_);
        if (!view_ || !projection_) {
            error = "Viewer editor camera buffers could not be created";
            return false;
        }
    }
    if (auto drawing = vieweredit::TakeDrawing ()) {
        scale_ = drawing->scale;
        if ((drawing->sceneChanged && !Upload (device, pipelines_, drawing->scene, pages_, scene_, error)) ||
            !Upload (device, pipelines_, drawing->hud, hudPages_, hud_, error))
            return false;
        ready_ = true;
    }
    if (!ready_)
        return true;
    if (!Matrix (context, view_, frame.view) || !Matrix (context, projection_, frame.projection)) {
        error = "Viewer editor camera upload failed";
        return false;
    }
    gpu::BindCamera (pipelines_, pages_, view_, projection_);
    gpu::BindCamera (pipelines_, hudPages_, view_, projection_);
    const float constants[] = { float (frame.width), float (frame.height), scale_, 0.00001f };
    gpu::DrawStats stats;
    gpu::Draw (context, pipelines_, pages_, scene_, constants, true, scale_, stats, {}, true);
    gpu::Draw (context, pipelines_, pages_, scene_, constants, true, scale_, stats);
    gpu::Draw (context, pipelines_, hudPages_, hud_, constants, true, scale_, stats);
    if (stats.mapFailures) {
        error = "Viewer editor draw constants upload failed";
        return false;
    }
    vieweredit::Drawn (generation_);
    return true;
}

void ViewerEditorDraw::Clear ()
{
    pipelines_ = {};
    scene_ = {};
    hud_ = {};
    pages_.clear ();
    hudPages_.clear ();
    view_.Release ();
    projection_.Release ();
    ready_ = false;
}
} // namespace geomsrv::archviz
