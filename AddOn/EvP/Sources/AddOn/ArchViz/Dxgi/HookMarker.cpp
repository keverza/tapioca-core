// ArchViz/Dxgi/HookMarker -- see the header for why the phase-3 gate draws a
// square and nothing else. RENDER THREAD except where marked.

#include "ArchViz/Dxgi/HookMarker.hpp"

#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_1.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {

namespace {

std::atomic<bool>     g_enabled {false};
std::atomic<uint64_t> g_target {0};
std::atomic<uint64_t> g_draws {0};
std::atomic<uint64_t> g_failures {0};

// The reason the last failure gave, as an index into `kStepNames`. An atomic
// index rather than a string: the render thread must not allocate, and a torn
// std::string read across threads is a crash rather than a bad message.
enum FailureStep {
    kNoFailure = 0,
    kGetBuffer,
    kGetDevice,
    kNoContext1,
    kCreateView
};
std::atomic<int> g_lastFailure {kNoFailure};

const char* StepName (int step)
{
    switch (step) {
        case kGetBuffer:   return "IDXGISwapChain::GetBuffer refused the back buffer";
        case kGetDevice:   return "the back buffer would not name its device";
        case kNoContext1:  return "the immediate context is not ID3D11DeviceContext1, so "
                                  "ClearView is unavailable (Windows 8 or later is required)";
        case kCreateView:  return "CreateRenderTargetView refused the back buffer";
        default:           return "";
    }
}

void Fail (FailureStep step)
{
    g_lastFailure.store (step, std::memory_order_relaxed);
    g_failures.fetch_add (1, std::memory_order_relaxed);
}

// ⚠️ FOUR SQUARES, ONE PER CORNER, AND THAT IS NOT DECORATION. The first
// version drew ONE at the top-left -- which is exactly where the overlay's own
// ImGui panel sits, so a square drawn perfectly into Archicad's back buffer
// would have been hidden behind our own window and reported as "did not appear"
// (2026-08-14). Anything drawn into the HOST's frame is BELOW everything we
// composite on top of it, so no single position is safe. Four corners also
// answer a question one square cannot: whether the injection covers the whole
// back buffer or only part of it.
constexpr int kMarkerInsetPixels = 80;
constexpr int kMarkerSizePixels = 96;

// Opaque red. Premultiplied alpha does not enter into it -- ClearView writes the
// colour into the back buffer directly, exactly as a clear does.
const float kMarkerColour[4] = {0.90f, 0.10f, 0.10f, 1.0f};

}   // namespace

void DrawMarkerIfTarget (IDXGISwapChain* swapChain)
{
    if (!g_enabled.load (std::memory_order_acquire) || swapChain == nullptr)
        return;
    if (uint64_t (uintptr_t (swapChain)) != g_target.load (std::memory_order_acquire))
        return;

    // ⚠️ EVERY COM POINTER BELOW IS RELEASED ON EVERY PATH, including the failure
    // paths. A leak here is a leaked D3D resource per frame at 60 Hz on
    // Archicad's own device: the symptom is Archicad slowly running out of video
    // memory, which nobody would connect to a red square.
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) ||
        backBuffer == nullptr) {
        Fail (kGetBuffer);
        return;
    }

    ID3D11Device* device = nullptr;
    backBuffer->GetDevice (&device);
    if (device == nullptr) {
        backBuffer->Release ();
        Fail (kGetDevice);
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext (&context);
    ID3D11DeviceContext1* context1 = nullptr;
    if (context != nullptr)
        context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1);

    if (context1 == nullptr) {
        if (context != nullptr)
            context->Release ();
        device->Release ();
        backBuffer->Release ();
        Fail (kNoContext1);
        return;
    }

    ID3D11RenderTargetView* view = nullptr;
    const HRESULT created = device->CreateRenderTargetView (backBuffer, nullptr, &view);
    if (SUCCEEDED (created) && view != nullptr) {
        // ⚠️ THE RECT IS CLAMPED TO THE BUFFER, not to a guess about the window.
        // ClearView with a rectangle outside the view is undefined, and a
        // minimised or freshly resized window is exactly when the two disagree.
        D3D11_TEXTURE2D_DESC desc = {};
        backBuffer->GetDesc (&desc);
        // ⚠️ NOT `near`/`far`: windows.h defines both as empty macros for 16-bit
        // compatibility, so a local of either name silently becomes nothing.
        const LONG inset = LONG (kMarkerInsetPixels);
        const LONG outer = LONG (kMarkerInsetPixels + kMarkerSizePixels);
        if (desc.Width > UINT (2 * outer) && desc.Height > UINT (2 * outer)) {
            const LONG rightEdge = LONG (desc.Width);
            const LONG bottomEdge = LONG (desc.Height);
            const D3D11_RECT rects[4] = {
                {inset, inset, outer, outer},
                {rightEdge - outer, inset, rightEdge - inset, outer},
                {inset, bottomEdge - outer, outer, bottomEdge - inset},
                {rightEdge - outer, bottomEdge - outer, rightEdge - inset, bottomEdge - inset}
            };
            context1->ClearView (view, kMarkerColour, rects, 4);
            g_draws.fetch_add (1, std::memory_order_relaxed);
        }
        view->Release ();
    } else {
        Fail (kCreateView);
    }

    context1->Release ();
    if (context != nullptr)
        context->Release ();
    device->Release ();
    backBuffer->Release ();
}

void SetMarkerEnabled (bool enabled)
{
    if (enabled) {
        // Same rule as the Present hook's: a run must never be able to read a
        // previous run's success.
        g_draws.store (0, std::memory_order_relaxed);
        g_failures.store (0, std::memory_order_relaxed);
        g_lastFailure.store (kNoFailure, std::memory_order_relaxed);
    }
    if (!enabled) {
        // ⚠️ THE TARGET IS CLEARED ON THE WAY OUT. A stale nomination surviving
        // a disarm would put the square back the instant the mode was armed
        // again -- into a swap chain that may have been destroyed and its
        // address reused, which is the one way this file can touch memory that
        // is no longer a swap chain.
        g_target.store (0, std::memory_order_release);
    }
    g_enabled.store (enabled, std::memory_order_release);
}

bool MarkerEnabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void SetMarkerTarget (uint64_t swapChain)
{
    g_target.store (swapChain, std::memory_order_release);
}

uint64_t MarkerTarget ()
{
    return g_target.load (std::memory_order_acquire);
}

void ChooseMarkerTargetIfUnset ()
{
    if (!g_enabled.load (std::memory_order_acquire) ||
        g_target.load (std::memory_order_acquire) != 0)
        return;
    const PresentStats stats = GetPresentStats ();
    // ⚠️ A HANDFUL OF FRAMES IS NOT AN IDENTIFICATION. The busiest chain over
    // three frames could be anything that happened to redraw; over sixty it is
    // the one that presents continuously. Waiting costs a second of the run and
    // buys the difference between "Archicad's window" and "whatever blinked".
    if (stats.busiestSwapChain == 0 || stats.busiestFrameCount < 60)
        return;

    // ⚠️ AND "BUSIEST" IS STILL ONLY AN INFERENCE. The overlay was put over ONE
    // document window, and that window's HWND is known exactly -- so the chain
    // is confirmed against it rather than assumed. Archicad has other D3D
    // surfaces (previews, other views); compositing into one of those would put
    // the overlay somewhere the user is not looking, and the run would report it
    // as a success. `target` is the document window the overlay covers, and a
    // swap chain may present into it or into a child of it.
    const HWND documentWindow = viewportoverlay::Stats ().target;
    const uint64_t chainWindow = SwapChainWindow (stats.busiestSwapChain);
    if (documentWindow != nullptr && chainWindow != 0) {
        const HWND presented = (HWND) (uintptr_t) chainWindow;
        if (presented != documentWindow && !IsChild (documentWindow, presented) &&
            GetAncestor (presented, GA_ROOT) != GetAncestor (documentWindow, GA_ROOT))
            return;   // the busiest chain is not the window we are over
    }
    g_target.store (stats.busiestSwapChain, std::memory_order_release);
}

MarkerStats GetMarkerStats ()
{
    MarkerStats stats;
    stats.enabled = g_enabled.load (std::memory_order_acquire);
    stats.target = g_target.load (std::memory_order_acquire);
    stats.draws = g_draws.load (std::memory_order_relaxed);
    stats.failures = g_failures.load (std::memory_order_relaxed);
    stats.lastError = StepName (g_lastFailure.load (std::memory_order_relaxed));
    return stats;
}

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
