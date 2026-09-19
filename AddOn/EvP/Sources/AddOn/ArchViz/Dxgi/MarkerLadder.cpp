// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and
// each cost at least one. Proof primitives are OFF in production (section 10),
// and a diagnostic may never change the timing of the thing it measures.

// See MarkerLadder.hpp.

#include "ArchViz/Dxgi/MarkerLadder.hpp"

#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace markerladder {
namespace {

constexpr size_t kRungs = size_t (Rung::Count);

std::atomic<bool> g_enabled { false };
std::atomic<uint64_t> g_painted[kRungs];
std::atomic<uint64_t> g_failures { 0 };

// ⚠️ A COLUMN DOWN THE LEFT EDGE, ONE SLOT PER RUNG, AND THE
// SLOT IS THE ANSWER. Distinct colours alone would need the reader to remember a
// legend; distinct POSITIONS mean a photograph of the screen is the result. Top
// to bottom is A, B, C, E -- the order the frame visits them.
constexpr UINT kInsetPixels = 24;
constexpr UINT kSizePixels = 56;
constexpr UINT kGapPixels = 12;

// A red, B yellow, C green, E magenta. Colour AND position, so a patch that
// lands somewhere unexpected is still identifiable.
const float kColour[kRungs][4] = {
    { 1.0f, 0.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 1.0f },
    { 0.0f, 1.0f, 0.0f, 1.0f },
    { 1.0f, 0.0f, 1.0f, 1.0f },
};

// ⚠️ THE RECT IS CLAMPED TO THE RESOURCE, NEVER TO A GUESS
// ABOUT THE WINDOW. `ClearView` with a rectangle outside the view is undefined,
// and a minimised or freshly resized target is exactly when the two disagree --
// the same rule `HookMarker` already carries.
bool RectFor (ID3D11RenderTargetView* view, Rung rung, D3D11_RECT& out)
{
    ID3D11Resource* resource = nullptr;
    view->GetResource (&resource);
    if (resource == nullptr)
        return false;

    ID3D11Texture2D* texture = nullptr;
    const HRESULT queried = resource->QueryInterface (__uuidof (ID3D11Texture2D), (void**) &texture);
    resource->Release ();
    if (FAILED (queried) || texture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc (&desc);
    texture->Release ();

    const UINT index = UINT (rung);
    const UINT top = kInsetPixels + index * (kSizePixels + kGapPixels);
    const UINT bottom = top + kSizePixels;
    const UINT right = kInsetPixels + kSizePixels;
    if (desc.Width <= right || desc.Height <= bottom)
        return false; // too small to carry the ladder; not a failure, just skipped

    out.left = LONG (kInsetPixels);
    out.top = LONG (top);
    out.right = LONG (right);
    out.bottom = LONG (bottom);
    return true;
}

} // namespace

const char* RungName (Rung rung)
{
    switch (rung) {
        case Rung::AfterModelDraw:
            return "A after the model draw (scene target)";
        case Rung::SceneBoundary:
            return "B at the scene boundary (scene target)";
        case Rung::NextTarget:
            return "C the target switched to (next target)";
        case Rung::BeforePresent:
            return "E before Present (back buffer)";
        case Rung::Count:
            break;
    }
    return "unknown";
}

void SetEnabled (bool enabled)
{
    if (enabled)
        Reset ();
    g_enabled.store (enabled, std::memory_order_release);
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

ID3D11RenderTargetView* BoundTarget (ID3D11DeviceContext* context)
{
    if (!Enabled () || context == nullptr)
        return nullptr;
    ID3D11RenderTargetView* view = nullptr;
    context->OMGetRenderTargets (1, &view, nullptr);
    return view; // AddRef'd by OMGetRenderTargets; the caller releases
}

void Paint (ID3D11DeviceContext* context, ID3D11RenderTargetView* view, Rung rung)
{
    if (!Enabled () || context == nullptr || view == nullptr || rung >= Rung::Count)
        return;

    // ⚠️ EVERY COM POINTER IS RELEASED ON EVERY PATH, including
    // the failures. A leak here is a leaked reference per frame at 60 Hz on
    // Archicad's own device, and the symptom -- Archicad slowly running out of
    // video memory -- is one nobody would connect to a coloured square.
    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) || context1 == nullptr) {
        g_failures.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    D3D11_RECT rect = {};
    if (RectFor (view, rung, rect)) {
        // ⚠️ `ClearView` BINDS NOTHING. It addresses the view
        // directly, so it needs no shader, no vertex buffer, no input layout and
        // no depth state, and it leaves the pipeline exactly as it found it.
        // Section 11 demands full state restoration for anything that draws;
        // never disturbing the state is the stronger guarantee.
        context1->ClearView (view, kColour[size_t (rung)], &rect, 1);
        g_painted[size_t (rung)].fetch_add (1, std::memory_order_relaxed);
    }
    context1->Release ();
}

void PaintSwapChain (IDXGISwapChain* swapChain, Rung rung)
{
    if (!Enabled () || swapChain == nullptr)
        return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) || backBuffer == nullptr) {
        g_failures.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    ID3D11Device* device = nullptr;
    backBuffer->GetDevice (&device);
    if (device == nullptr) {
        backBuffer->Release ();
        g_failures.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    ID3D11RenderTargetView* view = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (SUCCEEDED (device->CreateRenderTargetView (backBuffer, nullptr, &view)) && view != nullptr) {
        device->GetImmediateContext (&context);
        if (context != nullptr) {
            Paint (context, view, rung);
            context->Release ();
        }
        else {
            g_failures.fetch_add (1, std::memory_order_relaxed);
        }
        view->Release ();
    }
    else {
        g_failures.fetch_add (1, std::memory_order_relaxed);
    }

    device->Release ();
    backBuffer->Release ();
}

Stats GetStats ()
{
    Stats stats;
    for (size_t i = 0; i < kRungs; ++i)
        stats.painted[i] = g_painted[i].load (std::memory_order_relaxed);
    stats.failures = g_failures.load (std::memory_order_relaxed);
    return stats;
}

void Reset ()
{
    for (size_t i = 0; i < kRungs; ++i)
        g_painted[i].store (0, std::memory_order_relaxed);
    g_failures.store (0, std::memory_order_relaxed);
}

} // namespace markerladder
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
