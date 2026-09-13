// ArchViz/Dxgi/ContextStateTracker -- see the header. Every rule about this file
// is in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/ContextStateTracker.hpp"

#include <d3d11.h>

#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace contextstate {

namespace {

ContextState g_state;

}   // namespace

void OnVertexShader (ID3D11VertexShader* shader)
{
    g_state.vertexShader = uint64_t (uintptr_t (shader));
}

void OnVSConstantBuffers (uint32_t startSlot, uint32_t count, ID3D11Buffer* const* buffers,
                          const uint32_t* firstConstant, const uint32_t* numConstants)
{
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = startSlot + i;
        if (slot >= kConstantBufferSlots)
            break;
        ConstantBufferBinding& binding = g_state.vsConstantBuffers[slot];

        // ⚠️ A NULL BUFFER IS AN UNBIND, NOT A NO-OP. D3D11 uses it to clear a
        // slot, and leaving the previous pointer there would have a scene pass
        // inherit a binding Archicad had explicitly removed -- which is exactly
        // the class of wrong answer this tracker exists to stop.
        ID3D11Buffer* const buffer = (buffers != nullptr) ? buffers[i] : nullptr;
        binding.buffer = uint64_t (uintptr_t (buffer));
        binding.firstConstant = (firstConstant != nullptr) ? firstConstant[i] : 0;
        binding.numConstants = (numConstants != nullptr) ? numConstants[i] : 0;
    }
}

void OnRenderTargets (ID3D11RenderTargetView* colour, ID3D11DepthStencilView* depth)
{
    g_state.renderTarget = uint64_t (uintptr_t (colour));
    g_state.depthStencil = uint64_t (uintptr_t (depth));
}

void OnViewport (const D3D11_VIEWPORT& viewport)
{
    g_state.viewportX = viewport.TopLeftX;
    g_state.viewportY = viewport.TopLeftY;
    g_state.viewportWidth = viewport.Width;
    g_state.viewportHeight = viewport.Height;
}

ContextState Snapshot ()
{
    return g_state;
}

void Reset ()
{
    g_state = ContextState {};
}

}   // namespace contextstate
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
