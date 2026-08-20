#include "ArchViz/DiligentShadowMap.hpp"

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <Buffer.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <InputLayout.h>
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Shader.h>
#include <ShaderResourceBinding.h>
#include <Texture.h>
#include <TextureView.h>

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

struct DiligentShadowMap::Impl {
    RefCntAutoPtr<Diligent::ITexture> texture;
    RefCntAutoPtr<Diligent::IPipelineState> pso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    Diligent::ITextureView* dsv = nullptr;   // owned by `texture`
    Diligent::ITextureView* srv = nullptr;   // owned by `texture`
    uint32_t resolution = 0;
    bool ready = false;
};

DiligentShadowMap::DiligentShadowMap () : impl_ (new Impl ()) {}
DiligentShadowMap::~DiligentShadowMap ()
{
    Shutdown ();
    delete impl_;
}

bool DiligentShadowMap::IsReady () const { return impl_ != nullptr && impl_->ready; }
uint32_t DiligentShadowMap::Resolution () const { return impl_ != nullptr ? impl_->resolution : 0; }
Diligent::ITextureView* DiligentShadowMap::ShaderView () const
{
    return impl_ != nullptr ? impl_->srv : nullptr;
}
Diligent::IShaderResourceBinding* DiligentShadowMap::Srb () const
{
    return impl_ != nullptr ? impl_->srb.RawPtr () : nullptr;
}

bool DiligentShadowMap::Init (Diligent::IRenderDevice* device, Diligent::IShader* shadowVs,
                              Diligent::IBuffer* constants, const void* inputLayout,
                              uint32_t layoutElementCount, uint32_t resolution,
                              std::string& error)
{
    if (device == nullptr || shadowVs == nullptr || constants == nullptr ||
        inputLayout == nullptr || layoutElementCount == 0 || resolution == 0) {
        error = "DiligentShadowMap::Init got a null device, shader, layout or a zero resolution";
        return false;
    }
    if (impl_->ready)
        return true;

    Diligent::TextureDesc td;
    td.Name = "ArchViz sun shadow map";
    td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width = resolution;
    td.Height = resolution;
    td.MipLevels = 1;
    td.Format = Diligent::TEX_FORMAT_D32_FLOAT;
    // ⚠️ BOTH FLAGS. Diligent then creates the underlying D3D resource TYPELESS
    // and hands out a D32_FLOAT depth view and an R32_FLOAT shader view off the
    // same memory. With BIND_DEPTH_STENCIL alone the shader view comes back null
    // and the shadow term is silently 1 everywhere -- a scene with no shadows and
    // no error.
    td.BindFlags = Diligent::BIND_DEPTH_STENCIL | Diligent::BIND_SHADER_RESOURCE;
    td.Usage = Diligent::USAGE_DEFAULT;
    device->CreateTexture (td, nullptr, &impl_->texture);
    if (impl_->texture == nullptr) {
        error = "Diligent CreateTexture(ArchViz sun shadow map) failed";
        return false;
    }
    impl_->dsv = impl_->texture->GetDefaultView (Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    impl_->srv = impl_->texture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (impl_->dsv == nullptr || impl_->srv == nullptr) {
        error = "the shadow map has no depth view or no shader view -- the bind flags did not "
                "produce both";
        impl_->texture.Release ();
        return false;
    }

    Diligent::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = "ArchViz shadow depth PSO";
    Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
    // ⚠️ NO COLOUR TARGET AT ALL. A depth-only pass with NumRenderTargets = 1 and
    // a null RTV is a validation error on some drivers and a wasted bandwidth on
    // the rest; there is no pixel shader either, for the same reason.
    gp.NumRenderTargets = 0;
    gp.DSVFormat = Diligent::TEX_FORMAT_D32_FLOAT;
    gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // ⚠️ CULL_MODE_NONE, NOT front-face culling. Archicad's extracted meshes are
    // not reliably closed -- a wall with an opening, a stair, a railing -- and
    // culling in the depth pass punches a hole in the shadow of anything
    // one-sided. Peter-panning from double-sided depth is handled by the normal
    // offset in the pixel shader instead, which does not depend on the geometry
    // being watertight.
    gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
    gp.DepthStencilDesc.DepthEnable = Diligent::True;
    gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;
    gp.InputLayout.LayoutElements = static_cast<const Diligent::LayoutElement*> (inputLayout);
    gp.InputLayout.NumElements = layoutElementCount;

    pci.pVS = shadowVs;
    pci.pPS = nullptr;
    pci.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

    device->CreateGraphicsPipelineState (pci, &impl_->pso);
    if (impl_->pso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(ArchViz shadow depth) failed";
        impl_->texture.Release ();
        return false;
    }

    Diligent::IShaderResourceVariable* variable =
        impl_->pso->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX, "ArchVizConstants");
    if (variable == nullptr) {
        error = "the shadow vertex shader does not expose `ArchVizConstants` -- the light matrix "
                "would never reach it and the map would be rendered from the camera";
        impl_->pso.Release ();
        impl_->texture.Release ();
        return false;
    }
    variable->Set (constants);

    impl_->pso->CreateShaderResourceBinding (&impl_->srb, true);
    if (impl_->srb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(ArchViz shadow depth) failed";
        impl_->pso.Release ();
        impl_->texture.Release ();
        return false;
    }

    impl_->resolution = resolution;
    impl_->ready = true;
    return true;
}

void DiligentShadowMap::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->srb.Release ();
    impl_->pso.Release ();
    impl_->dsv = nullptr;
    impl_->srv = nullptr;
    impl_->texture.Release ();
    impl_->resolution = 0;
    impl_->ready = false;
}

void DiligentShadowMap::Begin (Diligent::IDeviceContext* context)
{
    if (context == nullptr || !impl_->ready)
        return;

    // ⚠️ UNBIND EVERY SHADER RESOURCE FIRST. Last frame's main pass left this
    // very texture bound as an SRV; D3D11 will not let the same subresource be
    // an SRV and a depth target at once, and it resolves the conflict by
    // silently dropping ONE of them. Which one it drops is not something to rely
    // on. TRANSITION mode makes Diligent do the unbinding explicitly.
    context->SetRenderTargets (0, nullptr, nullptr,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetRenderTargets (0, nullptr, impl_->dsv,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil (impl_->dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // ⚠️ AN EXPLICIT VIEWPORT, unlike the main pass. `SetViewports(1, nullptr,
    // 0, 0)` takes the dimensions of the bound render target, and there is no
    // colour target bound here -- so it would take the swap chain's size and
    // render the shadow map through a window the size of the palette.
    Diligent::Viewport viewport;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = float (impl_->resolution);
    viewport.Height = float (impl_->resolution);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->SetViewports (1, &viewport, impl_->resolution, impl_->resolution);

    context->SetPipelineState (impl_->pso);
    context->CommitShaderResources (impl_->srb,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void DiligentShadowMap::End (Diligent::IDeviceContext* context)
{
    if (context == nullptr || !impl_->ready)
        return;
    // Release the depth target so the texture can become a shader resource in
    // the same frame. The caller binds its own targets next; see the header.
    context->SetRenderTargets (0, nullptr, nullptr,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

}   // namespace archviz
}   // namespace geomsrv
