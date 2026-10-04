// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/Dxgi/PrelockHud -- see the header.

#include "ArchViz/Dxgi/PrelockHud.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/PipelineStateGuard.hpp"
#include "ArchViz/Dxgi/SceneGuest.hpp"

#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace prelockhud {

namespace {

std::atomic<uint64_t> s_drawn { 0 }, s_nothing { 0 }, s_reentrant { 0 }, s_noTarget { 0 }, s_failed { 0 };

void Bump (std::atomic<uint64_t>& counter)
{
    counter.fetch_add (1, std::memory_order_relaxed);
}

template <typename T> void Release (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

} // namespace

void DrawAtPresent (ID3D11DeviceContext* context, IDXGISwapChain* swapChain)
{
    if (context == nullptr || swapChain == nullptr)
        return;
    // Disabled: no overlay. Active: the composer draws the HUD with the camera (§2).
    if (injection::GetArmState () != injection::ArmState::ArmedPendingCamera)
        return;
    if (contextstate::Injecting ()) {
        Bump (s_reentrant);
        return;
    }
    // ⚠️ THE GUARD BEFORE ANY CALL: what follows reaches the hooked context, and none of it may
    // be recorded as Archicad's work -- the census learns the camera from exactly those records.
    contextstate::ScopedInjectionGuard guard;
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    if (SUCCEEDED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) &&
        context1 != nullptr) {
        context->GetDevice (&device);
        // ⚠️ FETCHED AND RELEASED HERE, NEVER HELD: a view of a swap-chain buffer that outlives
        // the Present makes ResizeBuffers fail (§11).
        if (device != nullptr &&
            SUCCEEDED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) &&
            backBuffer != nullptr) {
            backBuffer->GetDesc (&desc);
            device->CreateRenderTargetView (backBuffer, nullptr, &target);
        }
    }
    if (target == nullptr) {
        Bump (s_noTarget);
    }
    else {
        const ScopedPipelineState saved (context, context1);
        // The whole surface: no scene viewport is known without a camera, and the HUD is laid
        // out for the canvas it is drawn into (OverlayInput.hpp).
        D3D11_VIEWPORT viewport = {};
        viewport.Width = float (desc.Width);
        viewport.Height = float (desc.Height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports (1, &viewport);
        switch (sceneguest::DrawHudOnly (context, target, viewport.Width, viewport.Height)) {
            case sceneguest::HudOnly::Drawn:
                Bump (s_drawn);
                break;
            case sceneguest::HudOnly::Nothing:
                Bump (s_nothing);
                break;
            case sceneguest::HudOnly::NoTarget:
                Bump (s_noTarget);
                break;
            case sceneguest::HudOnly::Failed:
                Bump (s_failed);
                break;
        }
    }
    Release (target);
    Release (backBuffer);
    Release (device);
    Release (context1);
}

Stats GetStats ()
{
    Stats stats;
    stats.drawn = s_drawn.load (std::memory_order_relaxed);
    stats.nothing = s_nothing.load (std::memory_order_relaxed);
    stats.reentrant = s_reentrant.load (std::memory_order_relaxed);
    stats.noTarget = s_noTarget.load (std::memory_order_relaxed);
    stats.failed = s_failed.load (std::memory_order_relaxed);
    return stats;
}

} // namespace prelockhud
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
