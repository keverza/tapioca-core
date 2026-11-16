// Bound by OVERLAY-INVARIANTS.md: a Present-time display pass, never a camera,
// depth, interception, or persistent Archicad-resource change.
#include "ArchViz/Dxgi/SurfaceFocusContext.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace geomsrv::archviz::dxgi::surfacefocuscontext {
namespace {

ID3D11VertexShader* g_vertexShader = nullptr;
ID3D11PixelShader* g_pixelShader = nullptr;
ID3D11Buffer* g_opacity = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Texture2D* g_copy = nullptr;
ID3D11ShaderResourceView* g_copyView = nullptr;
D3D11_TEXTURE2D_DESC g_copyDesc = {};
bool g_createFailed = false;

template <typename T> void Release (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

bool Compile (ID3D11Device* device)
{
    if (g_vertexShader != nullptr && g_pixelShader != nullptr && g_opacity != nullptr && g_raster != nullptr)
        return true;
    if (g_createFailed)
        return false;

    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ID3DBlob* vertex = nullptr;
    ID3DBlob* pixel = nullptr;
    ID3DBlob* errors = nullptr;
    bool ok = SUCCEEDED (D3DCompile (kVertexShader, std::strlen (kVertexShader), "Tapioca surface context", nullptr,
                                     nullptr, "VSSurfaceContext", "vs_5_0", flags, 0, &vertex, &errors));
    Release (errors);
    if (ok)
        ok = SUCCEEDED (D3DCompile (kPixelShader, std::strlen (kPixelShader), "Tapioca surface context", nullptr,
                                    nullptr, "PSSurfaceContext", "ps_5_0", flags, 0, &pixel, &errors));
    Release (errors);
    if (ok)
        ok = SUCCEEDED (device->CreateVertexShader (vertex->GetBufferPointer (), vertex->GetBufferSize (), nullptr,
                                                    &g_vertexShader));
    if (ok)
        ok = SUCCEEDED (
            device->CreatePixelShader (pixel->GetBufferPointer (), pixel->GetBufferSize (), nullptr, &g_pixelShader));
    Release (vertex);
    Release (pixel);

    D3D11_BUFFER_DESC opacity = {};
    opacity.ByteWidth = 16;
    opacity.Usage = D3D11_USAGE_DYNAMIC;
    opacity.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    opacity.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (ok)
        ok = SUCCEEDED (device->CreateBuffer (&opacity, nullptr, &g_opacity));
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (ok)
        ok = SUCCEEDED (device->CreateRasterizerState (&raster, &g_raster));
    if (!ok) {
        g_createFailed = true;
        Release (g_vertexShader);
        Release (g_pixelShader);
        Release (g_opacity);
        Release (g_raster);
    }
    return ok;
}

bool CopyMatches (const D3D11_TEXTURE2D_DESC& desc)
{
    return g_copy != nullptr && g_copyView != nullptr && g_copyDesc.Width == desc.Width &&
           g_copyDesc.Height == desc.Height && g_copyDesc.Format == desc.Format;
}

bool PrepareCopy (ID3D11Device* device, const D3D11_TEXTURE2D_DESC& target)
{
    if (CopyMatches (target))
        return true;
    Release (g_copyView);
    Release (g_copy);
    g_copyDesc = {};

    D3D11_TEXTURE2D_DESC copy = {};
    copy.Width = target.Width;
    copy.Height = target.Height;
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    copy.Format = target.Format;
    copy.SampleDesc.Count = 1;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED (device->CreateTexture2D (&copy, nullptr, &g_copy)))
        return false;
    if (FAILED (device->CreateShaderResourceView (g_copy, nullptr, &g_copyView))) {
        Release (g_copy);
        return false;
    }
    g_copyDesc = copy;
    return true;
}

bool UploadOpacity (ID3D11DeviceContext* context, float opacity)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED (context->Map (g_opacity, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    const float values[4] = { std::clamp (opacity, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f };
    std::memcpy (mapped.pData, values, sizeof (values));
    context->Unmap (g_opacity, 0);
    return true;
}

} // namespace

DrawResult Draw (ID3D11DeviceContext* context, ID3D11RenderTargetView* target, float opacity)
{
    if (context == nullptr || target == nullptr)
        return DrawResult::NoTarget;

    ID3D11Resource* resource = nullptr;
    target->GetResource (&resource);
    ID3D11Texture2D* source = nullptr;
    if (resource != nullptr)
        resource->QueryInterface (__uuidof (ID3D11Texture2D), reinterpret_cast<void**> (&source));
    Release (resource);
    if (source == nullptr)
        return DrawResult::UnsupportedTarget;

    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc (&desc);
    if (desc.Width == 0 || desc.Height == 0 || desc.Format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 || desc.ArraySize != 1) {
        Release (source);
        return DrawResult::UnsupportedTarget;
    }

    ID3D11Device* device = nullptr;
    context->GetDevice (&device);
    const bool ready = device != nullptr && Compile (device) && PrepareCopy (device, desc);
    Release (device);
    if (!ready) {
        Release (source);
        return DrawResult::PipelineError;
    }

    // Do not copy from a texture still bound as the draw target. The caller's
    // guard restores Archicad's original bindings after the complete composition.
    context->OMSetRenderTargets (0, nullptr, nullptr);
    context->CopyResource (g_copy, source);
    Release (source);
    if (!UploadOpacity (context, opacity))
        return DrawResult::PipelineError;

    context->OMSetRenderTargets (1, &target, nullptr);
    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, float (desc.Width), float (desc.Height), 0.0f, 1.0f };
    context->RSSetViewports (1, &viewport);
    context->IASetInputLayout (nullptr);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetVertexBuffers (0, 0, nullptr, nullptr, nullptr);
    context->IASetIndexBuffer (nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->VSSetShader (g_vertexShader, nullptr, 0);
    context->HSSetShader (nullptr, nullptr, 0);
    context->DSSetShader (nullptr, nullptr, 0);
    context->GSSetShader (nullptr, nullptr, 0);
    context->PSSetShader (g_pixelShader, nullptr, 0);
    context->RSSetState (g_raster);
    context->PSSetShaderResources (0, 1, &g_copyView);
    context->PSSetConstantBuffers (0, 1, &g_opacity);
    context->OMSetBlendState (nullptr, nullptr, 0xffffffffu);
    context->OMSetDepthStencilState (nullptr, 0);
    context->Draw (3, 0);

    ID3D11ShaderResourceView* const noView = nullptr;
    context->PSSetShaderResources (0, 1, &noView);
    ID3D11Buffer* const noConstants = nullptr;
    context->PSSetConstantBuffers (0, 1, &noConstants);
    return DrawResult::Drawn;
}

void Shutdown ()
{
    Release (g_copyView);
    Release (g_copy);
    Release (g_raster);
    Release (g_opacity);
    Release (g_pixelShader);
    Release (g_vertexShader);
    g_copyDesc = {};
    g_createFailed = false;
}

} // namespace geomsrv::archviz::dxgi::surfacefocuscontext
