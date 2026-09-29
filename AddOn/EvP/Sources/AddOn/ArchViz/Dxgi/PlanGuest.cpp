// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. The plan's Present only binds and draws; everything is made on
// the tick, outside it; the transform is the one read at the Present (§11, finding 14).
// ArchViz/Dxgi/PlanGuest -- see the header.

#include "ArchViz/Dxgi/PlanGuest.hpp"

#include "ArchViz/Dxgi/DiligentGuest.hpp"
#include "ArchViz/Dxgi/GuestGpu.hpp"
#include "ArchViz/Dxgi/GuestShaderSources.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <chrono>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace planguest {

namespace {

namespace gpu = guestgpu;

// `cbuffer GuestPlanFrame`.
struct Frame {
    float linear[4];
    float view[4];
    float screen[4];
    float surface[4];
};
static_assert (sizeof (Frame) == 64, "GuestPlanFrame is four float4s");

DiligentGuest g_guest;
gpu::Pipelines g_pipelines;
std::vector<gpu::Page> g_pages;
gpu::Content g_content;
double g_originX = 0.0;
double g_originY = 0.0;
float g_dpiScale = 1.0f;
bool g_haveGeneration = false;
uint64_t g_generation = 0;
bool g_buildFailed = false;
// ⚠️ A FAILED ATTACH STAYS FAILED FOR THE SESSION: it allocates, and the tick runs ten
// times a second. The next start tries again (§8).
bool g_attachFailed = false;
Stats g_stats;

uint32_t Milliseconds (std::chrono::steady_clock::time_point since)
{
    return uint32_t (
        std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - since).count ());
}

void ReleaseDeviceObjects ()
{
    g_content = gpu::Content {};
    g_pages.clear ();
    g_pipelines = gpu::Pipelines {};
    g_buildFailed = false;
}

bool NeedsText (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers)
{
    for (const auto& layer : layers)
        if (overlaylayers::DrawnIn (layer->views, overlaylayers::Views::TwoD) &&
            (!layer->texts.empty () || !layer->dimensions.empty () || !layer->legends.empty ()))
            return true;
    return false;
}

bool NeedsGuest2D (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers)
{
    for (const auto& layer : layers)
        if (overlaylayers::DrawnIn (layer->views, overlaylayers::Views::TwoD) && overlaylayers::NeedsGuest (*layer))
            return true;
    return false;
}

} // namespace

bool Prepare (ID3D11Device* device, const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers,
              uint64_t generation, float dpiScale, bool& changed, std::string& error)
{
    changed = false;
    g_dpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;
    if (g_haveGeneration && generation == g_generation && !g_guest.DeviceChanged (device))
        return true;

    // Nothing for the guest: whatever it held goes, and it stays detached.
    if (!NeedsGuest2D (layers)) {
        changed = !g_content.Empty ();
        g_content = gpu::Content {};
        g_generation = generation;
        g_haveGeneration = true;
        return true;
    }
    if (device == nullptr) {
        error = "the plan's device is not known yet";
        return false;
    }
    // A chain that presents with another device: everything made for the old one goes
    // first -- a Diligent object outliving its device is the one thing never allowed.
    if (g_guest.DeviceChanged (device)) {
        ReleaseDeviceObjects ();
        g_guest.Detach ();
    }
    if (!g_guest.Attached ()) {
        if (g_attachFailed) {
            error = g_stats.lastError;
            return false;
        }
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext (&context);
        const bool attached = g_guest.Attach (device, context, error);
        if (context != nullptr)
            context->Release ();
        if (!attached) {
            g_attachFailed = true;
            g_stats.lastError = error;
            return false;
        }
        g_stats.attached = true;
        g_stats.attachMilliseconds = g_guest.GetStats ().attachMilliseconds;
    }
    if (!g_pipelines.ready) {
        if (g_buildFailed) {
            error = g_stats.lastError;
            return false;
        }
        const auto started = std::chrono::steady_clock::now ();
        if (!gpu::Build (g_guest.Device (), gpu::Kind::Plan, guestshaders::PlanSource (), g_pipelines, error)) {
            g_buildFailed = true;
            g_stats.lastError = error;
            return false;
        }
        g_stats.buildMilliseconds = Milliseconds (started);
    }

    overlaytext::Engine* text = NeedsText (layers) ? guesttext::Engine () : nullptr;
    const overlayscene::Plan plan = overlayscene::PreparePlan (layers, text);
    gpu::Arrays arrays;
    arrays.fills = plan.fills.data ();
    arrays.fillCount = plan.fills.size ();
    arrays.fillStride = sizeof (overlayscene::PlanFillVertex);
    arrays.lines = plan.lines.data ();
    arrays.lineCount = plan.lines.size ();
    arrays.lineStride = sizeof (overlayscene::PlanLine);
    arrays.glyphs = plan.glyphs.data ();
    arrays.glyphCount = plan.glyphs.size ();
    arrays.glyphStride = sizeof (overlayscene::PlanGlyph);
    gpu::Content content;
    if (!gpu::Upload (g_guest.Device (), g_pipelines, g_pages, arrays, plan.fillDraws, plan.glyphDraws, plan.pages,
                      content, error)) {
        g_stats.lastError = error;
        return false;
    }
    content.generation = generation;
    g_content = std::move (content);
    g_originX = plan.originX;
    g_originY = plan.originY;
    g_generation = generation;
    g_haveGeneration = true;
    changed = true;
    ++g_stats.uploads;
    g_stats.fills = uint32_t (plan.fills.size ());
    g_stats.lines = uint32_t (plan.lines.size ());
    g_stats.glyphVertices = uint32_t (plan.glyphs.size ());
    g_stats.pages = uint32_t (g_pages.size ());
    g_stats.textsNotLaidOut = plan.problems.textsNotLaidOut;
    g_stats.dimensionsNotResolved = plan.problems.dimensionsNotResolved;
    g_stats.truncated = plan.problems.truncated;
    if (!plan.problems.lastError.empty ())
        g_stats.lastError = plan.problems.lastError;
    return true;
}

bool HasContent ()
{
    return g_pipelines.ready && !g_content.Empty ();
}

void Draw (ID3D11DeviceContext* native, ID3D11RenderTargetView* target, const plancontent::PixelTransform& transform,
           uint32_t width, uint32_t height)
{
    if (!HasContent () || native == nullptr || target == nullptr)
        return;
    plancontent::ViewConstants view;
    if (!plancontent::MakeViewConstants (transform, g_originX, g_originY, width, height, view)) {
        ++g_stats.declinedNoTransform;
        return;
    }
    Frame frame = {};
    std::memcpy (frame.linear, view.linear, sizeof (frame.linear));
    std::memcpy (frame.view, view.view, sizeof (frame.view));
    std::memcpy (frame.screen, view.screen, sizeof (frame.screen));
    frame.surface[0] = float (width);
    frame.surface[1] = float (height);
    frame.surface[2] = g_dpiScale;
    // The plan layer set the viewport to the whole buffer; InvalidateState leaves it.
    g_guest.BeginDraw (native, target, nullptr);
    gpu::DrawStats drawn;
    gpu::Draw (g_guest.Context (), g_pipelines, g_pages, g_content, &frame, false, g_dpiScale, drawn);
    ++g_stats.draws;
    g_stats.drawCalls += drawn.drawCalls;
}

void Release ()
{
    ReleaseDeviceObjects ();
    g_guest.Detach ();
    g_attachFailed = false;
    g_haveGeneration = false;
    g_generation = 0;
    g_stats = Stats {};
}

Stats GetStats ()
{
    return g_stats;
}

} // namespace planguest
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
