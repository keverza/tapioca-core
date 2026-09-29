// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. Composition stays at Present, the camera is the census's copy
// read through ArchicadClip, and nothing here holds an Archicad resource (§11, §12b).
// ArchViz/Dxgi/SceneGuest -- see the header.

#include "ArchViz/Dxgi/SceneGuest.hpp"

#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/DiligentGuest.hpp"
#include "ArchViz/Dxgi/GuestGpu.hpp"
#include "ArchViz/Dxgi/GuestShaderSources.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <RenderDeviceD3D11.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace sceneguest {

namespace {

namespace gpu = guestgpu;

// ---- the hand-over --------------------------------------------------------------
struct Published {
    overlayscene::Scene scene;
    float dpiScale = 1.0f;
};
std::atomic<Published*> g_published { nullptr };
std::atomic<Published*> g_publishedHud { nullptr };

// ---- RENDER THREAD ----------------------------------------------------------------
std::unique_ptr<Published> g_current;
bool g_dirty = false;
// The HUD's stream: its own content and its own page cache, so neither upload drops the
// other's pages (GuestGpu's cache keeps only what the content it uploads samples).
std::unique_ptr<Published> g_currentHud;
bool g_hudDirty = false;
std::vector<gpu::Page> g_hudPages;
gpu::Content g_hudContent;
DiligentGuest g_guest;
gpu::Pipelines g_pipelines;
uint32_t g_slot = UINT32_MAX;
std::vector<gpu::Page> g_pages;
gpu::Content g_content;
bool g_attachFailed = false;
bool g_buildFailed[camerashader::kShaderSlots] = {};
// The camera copies as Diligent sees them, and the native buffers they wrap: our
// own snapshot buffers, remade per injection session, so re-wrapped when they move.
ID3D11Buffer* g_viewNative = nullptr;
ID3D11Buffer* g_projectionNative = nullptr;
Diligent::RefCntAutoPtr<Diligent::IBuffer> g_view;
Diligent::RefCntAutoPtr<Diligent::IBuffer> g_projection;

// ---- read from the main thread ------------------------------------------------------
std::atomic<bool> s_attached { false };
std::atomic<uint32_t> s_attachMs { 0 }, s_buildMs { 0 }, s_slot { 0 };
std::atomic<uint64_t> s_draws { 0 }, s_drawCalls { 0 }, s_uploads { 0 };
std::atomic<uint64_t> s_noCamera { 0 }, s_noViewport { 0 }, s_failed { 0 };
std::atomic<uint32_t> s_fills { 0 }, s_lines { 0 }, s_glyphs { 0 }, s_pages { 0 };
std::atomic<const char*> s_failure { "" };
std::atomic<uint32_t> s_prepareUs { 0 }, s_built { 0 }, s_reused { 0 }, s_lastDrawUs { 0 }, s_drawUs { 0 };
std::atomic<uint64_t> s_vertexBytes { 0 }, s_pageBytes { 0 };
std::atomic<uint64_t> s_hudUploads { 0 };
std::atomic<uint32_t> s_hudGlyphs { 0 }, s_hudPrepareUs { 0 };

uint32_t Since (std::chrono::steady_clock::time_point started)
{
    return uint32_t (
        std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - started).count ());
}

void Bump (std::atomic<uint64_t>& counter)
{
    counter.fetch_add (1, std::memory_order_relaxed);
}

void ReleaseRenderObjects ()
{
    g_content = gpu::Content {};
    g_pages.clear ();
    g_hudContent = gpu::Content {};
    g_hudPages.clear ();
    g_pipelines = gpu::Pipelines {};
    g_slot = UINT32_MAX;
    g_view.Release ();
    g_projection.Release ();
    g_viewNative = nullptr;
    g_projectionNative = nullptr;
    g_dirty = g_current != nullptr;
    g_hudDirty = g_currentHud != nullptr;
}

bool EnsureAttached (ID3D11DeviceContext* context)
{
    ID3D11Device* device = nullptr;
    context->GetDevice (&device);
    if (device == nullptr)
        return false;
    if (g_guest.DeviceChanged (device)) {
        ReleaseRenderObjects ();
        g_guest.Detach ();
        g_attachFailed = false;
    }
    bool ok = g_guest.Attached ();
    if (!ok && !g_attachFailed) {
        std::string error;
        ok = g_guest.Attach (device, context, error);
        if (ok) {
            s_attached.store (true, std::memory_order_relaxed);
            s_attachMs.store (g_guest.GetStats ().attachMilliseconds, std::memory_order_relaxed);
        }
        else {
            g_attachFailed = true;
            s_failure.store ("the Diligent attach to Archicad's device failed", std::memory_order_relaxed);
        }
    }
    device->Release ();
    return ok;
}

bool EnsurePipelines (uint32_t slot)
{
    if (g_pipelines.ready && g_slot == slot)
        return true;
    if (g_buildFailed[slot])
        return false;
    // A different slot: its own shaders, so its own pipelines and page bindings.
    ReleaseRenderObjects ();
    const auto started = std::chrono::steady_clock::now ();
    const std::string body = guestshaders::SceneBody ();
    std::vector<char> source (body.size () + 4096);
    std::string error;
    if (!camerashader::Compose (camerashader::InterpretationOfSlot (slot), body.c_str (), source.data (),
                                source.size ()) ||
        !gpu::Build (g_guest.Device (), gpu::Kind::Scene, std::string (source.data ()), g_pipelines, error)) {
        g_buildFailed[slot] = true;
        s_failure.store ("the overlay guest's 3D pipelines could not be built", std::memory_order_relaxed);
        return false;
    }
    g_slot = slot;
    g_dirty = true;
    s_slot.store (slot, std::memory_order_relaxed);
    s_buildMs.store (
        uint32_t (std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - started)
                      .count ()),
        std::memory_order_relaxed);
    return true;
}

bool UploadScene (const overlayscene::Scene& scene, std::vector<gpu::Page>& pages, gpu::Content& out)
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
    std::string error;
    gpu::Content content;
    if (!gpu::Upload (g_guest.Device (), g_pipelines, pages, arrays, scene.fillDraws, scene.glyphDraws, scene.pages,
                      content, error)) {
        s_failure.store ("the overlay guest's 3D buffers could not be created", std::memory_order_relaxed);
        return false;
    }
    out = std::move (content);
    // New bindings read no camera yet.
    g_viewNative = nullptr;
    g_projectionNative = nullptr;
    return true;
}

bool UploadCurrent ()
{
    const overlayscene::Scene& scene = g_current->scene;
    if (!UploadScene (scene, g_pages, g_content))
        return false;
    s_fills.store (uint32_t (scene.fills.size ()), std::memory_order_relaxed);
    s_lines.store (uint32_t (scene.lines.size ()), std::memory_order_relaxed);
    s_glyphs.store (uint32_t (scene.glyphs.size ()), std::memory_order_relaxed);
    s_pages.store (uint32_t (g_pages.size ()), std::memory_order_relaxed);
    s_vertexBytes.store (uint64_t (scene.fills.size ()) * sizeof (overlayscene::SceneFillVertex) +
                             uint64_t (scene.lines.size ()) * sizeof (overlayscene::SceneLine) +
                             uint64_t (scene.glyphs.size ()) * sizeof (overlayscene::SceneGlyph),
                         std::memory_order_relaxed);
    uint64_t pageBytes = 0;
    for (const auto& page : scene.pages)
        pageBytes += page != nullptr ? uint64_t (page->width) * uint64_t (page->height) * 4u : 0u;
    s_pageBytes.store (pageBytes, std::memory_order_relaxed);
    Bump (s_uploads);
    return true;
}

bool UploadHud ()
{
    if (!UploadScene (g_currentHud->scene, g_hudPages, g_hudContent))
        return false;
    s_hudGlyphs.store (uint32_t (g_currentHud->scene.glyphs.size ()), std::memory_order_relaxed);
    Bump (s_hudUploads);
    return true;
}

bool BindCamera (ID3D11Buffer* view, ID3D11Buffer* projection)
{
    if (view == g_viewNative && projection == g_projectionNative && g_view != nullptr)
        return true;
    Diligent::RefCntAutoPtr<Diligent::IRenderDeviceD3D11> device (g_guest.Device (), Diligent::IID_RenderDeviceD3D11);
    if (device == nullptr)
        return false;
    g_view.Release ();
    g_projection.Release ();
    const Diligent::BufferDesc described; // taken from the native buffers
    device->CreateBufferFromD3DResource (view, described, Diligent::RESOURCE_STATE_CONSTANT_BUFFER, &g_view);
    device->CreateBufferFromD3DResource (projection, described, Diligent::RESOURCE_STATE_CONSTANT_BUFFER,
                                         &g_projection);
    if (g_view == nullptr || g_projection == nullptr)
        return false;
    gpu::BindCamera (g_pipelines, g_pages, g_view, g_projection);
    gpu::BindCamera (g_pipelines, g_hudPages, g_view, g_projection);
    g_viewNative = view;
    g_projectionNative = projection;
    return true;
}

} // namespace

void Publish (overlayscene::Scene scene, float dpiScale)
{
    s_prepareUs.store (scene.cost.microseconds, std::memory_order_relaxed);
    s_built.store (scene.cost.layersBuilt, std::memory_order_relaxed);
    s_reused.store (scene.cost.layersReused, std::memory_order_relaxed);
    Published* const fresh = new Published { std::move (scene), dpiScale };
    // One the render thread never took is simply superseded.
    delete g_published.exchange (fresh, std::memory_order_acq_rel);
}

void PublishHud (overlayscene::Scene hud, float dpiScale)
{
    s_hudPrepareUs.store (hud.cost.microseconds, std::memory_order_relaxed);
    Published* const fresh = new Published { std::move (hud), dpiScale };
    delete g_publishedHud.exchange (fresh, std::memory_order_acq_rel);
}

void Draw (ID3D11DeviceContext* context, uint32_t interpretation, ID3D11RenderTargetView* target,
           ID3D11DepthStencilView* depth)
{
    if (Published* const fresh = g_published.exchange (nullptr, std::memory_order_acq_rel)) {
        g_current.reset (fresh);
        g_dirty = true;
    }
    if (Published* const fresh = g_publishedHud.exchange (nullptr, std::memory_order_acq_rel)) {
        g_currentHud.reset (fresh);
        g_hudDirty = true;
    }
    const bool scene = g_current != nullptr && !g_current->scene.Empty ();
    const bool hud = g_currentHud != nullptr && !g_currentHud->scene.Empty ();
    if (!scene && g_dirty) {
        g_content = gpu::Content {};
        g_dirty = false;
    }
    if (!hud && g_hudDirty) {
        g_hudContent = gpu::Content {};
        g_hudDirty = false;
    }
    if (!scene && !hud)
        return;
    if (context == nullptr || target == nullptr)
        return;
    ID3D11Buffer* const view = injection::ViewSnapshotBuffer ();
    ID3D11Buffer* const projection = injection::ProjectionSnapshotBuffer ();
    if (view == nullptr || projection == nullptr || !injection::SnapshotValid ()) {
        Bump (s_noCamera);
        return;
    }
    const uint32_t slot = camerashader::Declarable (interpretation) ? camerashader::SlotOf (interpretation) : 0u;
    if (!EnsureAttached (context) || !EnsurePipelines (slot)) {
        Bump (s_failed);
        return;
    }
    if (g_dirty) {
        if (!UploadCurrent ()) {
            Bump (s_failed);
            return;
        }
        g_dirty = false;
    }
    if (g_hudDirty) {
        if (!UploadHud ()) {
            Bump (s_failed);
            return;
        }
        g_hudDirty = false;
    }
    if (!BindCamera (view, projection)) {
        Bump (s_noCamera);
        return;
    }
    D3D11_VIEWPORT viewport = {};
    UINT viewports = 1;
    context->RSGetViewports (&viewports, &viewport);
    if (viewports == 0 || viewport.Width < 1.0f || viewport.Height < 1.0f) {
        Bump (s_noViewport);
        return;
    }
    const float dpiScale = scene ? g_current->dpiScale : g_currentHud->dpiScale;
    const float frame[4] = { viewport.Width, viewport.Height, dpiScale, overlay::kGuestDepthPullFraction };
    // InvalidateState inside the injection's guard, then Archicad's target and the
    // composer's depth view, bound natively; the scene viewport stays as bound.
    const auto started = std::chrono::steady_clock::now ();
    g_guest.BeginDraw (context, target, depth);
    gpu::DrawStats drawn;
    // The band of a heatmap the HUD's pointer is on travels with the HUD.
    const overlayscene::Highlight highlight = hud ? g_currentHud->scene.highlight : overlayscene::Highlight ();
    gpu::Draw (g_guest.Context (), g_pipelines, g_pages, g_content, frame, depth != nullptr, dpiScale, drawn,
               highlight);
    // The HUD last, over everything.
    gpu::Draw (g_guest.Context (), g_pipelines, g_hudPages, g_hudContent, frame, depth != nullptr, dpiScale, drawn);
    // The render thread's own time for it -- lock-free, no allocation (§11).
    const uint32_t took = Since (started);
    s_lastDrawUs.store (took, std::memory_order_relaxed);
    const uint32_t mean = s_drawUs.load (std::memory_order_relaxed);
    s_drawUs.store (mean == 0 ? took : mean - mean / 16 + took / 16, std::memory_order_relaxed);
    Bump (s_draws);
    s_drawCalls.fetch_add (drawn.drawCalls, std::memory_order_relaxed);
}

void ReleaseDeviceObjects ()
{
    ReleaseRenderObjects ();
    g_guest.Detach ();
    g_attachFailed = false;
    for (bool& failed : g_buildFailed)
        failed = false;
    // Every Start resets what every Stop leaves behind (§8): the next session reports
    // its own numbers and its own failure, not these.
    s_attached.store (false, std::memory_order_relaxed);
    for (std::atomic<uint32_t>* value :
         { &s_attachMs, &s_buildMs, &s_slot, &s_fills, &s_lines, &s_glyphs, &s_pages, &s_prepareUs, &s_built, &s_reused,
           &s_lastDrawUs, &s_drawUs, &s_hudGlyphs, &s_hudPrepareUs })
        value->store (0, std::memory_order_relaxed);
    s_vertexBytes.store (0, std::memory_order_relaxed);
    s_pageBytes.store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>* counter :
         { &s_draws, &s_drawCalls, &s_uploads, &s_noCamera, &s_noViewport, &s_failed, &s_hudUploads })
        counter->store (0, std::memory_order_relaxed);
    s_failure.store ("", std::memory_order_relaxed);
}

Stats GetStats ()
{
    Stats stats;
    stats.attached = s_attached.load (std::memory_order_relaxed);
    stats.attachMilliseconds = s_attachMs.load (std::memory_order_relaxed);
    stats.buildMilliseconds = s_buildMs.load (std::memory_order_relaxed);
    stats.slot = s_slot.load (std::memory_order_relaxed);
    stats.draws = s_draws.load (std::memory_order_relaxed);
    stats.drawCalls = s_drawCalls.load (std::memory_order_relaxed);
    stats.uploads = s_uploads.load (std::memory_order_relaxed);
    stats.declinedNoCamera = s_noCamera.load (std::memory_order_relaxed);
    stats.declinedNoViewport = s_noViewport.load (std::memory_order_relaxed);
    stats.declinedFailed = s_failed.load (std::memory_order_relaxed);
    stats.fills = s_fills.load (std::memory_order_relaxed);
    stats.lines = s_lines.load (std::memory_order_relaxed);
    stats.glyphVertices = s_glyphs.load (std::memory_order_relaxed);
    stats.pages = s_pages.load (std::memory_order_relaxed);
    stats.failure = s_failure.load (std::memory_order_relaxed);
    stats.prepareMicroseconds = s_prepareUs.load (std::memory_order_relaxed);
    stats.layersBuilt = s_built.load (std::memory_order_relaxed);
    stats.layersReused = s_reused.load (std::memory_order_relaxed);
    stats.vertexBytes = s_vertexBytes.load (std::memory_order_relaxed);
    stats.pageBytes = s_pageBytes.load (std::memory_order_relaxed);
    stats.lastDrawMicroseconds = s_lastDrawUs.load (std::memory_order_relaxed);
    stats.drawMicroseconds = s_drawUs.load (std::memory_order_relaxed);
    stats.hudUploads = s_hudUploads.load (std::memory_order_relaxed);
    stats.hudGlyphVertices = s_hudGlyphs.load (std::memory_order_relaxed);
    stats.hudPrepareMicroseconds = s_hudPrepareUs.load (std::memory_order_relaxed);
    return stats;
}

} // namespace sceneguest
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
