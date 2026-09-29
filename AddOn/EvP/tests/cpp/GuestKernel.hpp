#ifndef EVP_TESTS_GUESTKERNEL_HPP
#define EVP_TESTS_GUESTKERNEL_HPP

// Runs a compute kernel appended to the guest's composed 3D source on WARP, the
// software device every Windows has -- so a test calls the shader's OWN functions
// (TowardEye, HaloReach) with numbers it chose, instead of a copy of them in C++.
//
// The kernel is an entry point `CSMain` reading `StructuredBuffer<float4> Input :
// register (t8)` and writing `RWStructuredBuffer<float4> Output : register (u0)`,
// `outputsPerInput` values per input, one thread per input. Archicad's two camera
// buffers are b1 and b2 as in production; the guest's own constant buffers have no
// register, so each is found by reflection and filled by variable name.

#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/GuestShaderSources.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace guestkernel {

using Microsoft::WRL::ComPtr;
using Float4 = std::array<float, 4>;

struct Inputs {
    uint32_t interpretation = 0;
    float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };       // b1, row-major as uploaded
    float projection[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // b2
    // Variables of the guest's constant buffers, by name: GuestSceneFrame's "Surface",
    // GuestDraw's "Mode", "Atlas", ...
    std::vector<std::pair<std::string, std::vector<float>>> variables;
};

inline ComPtr<ID3D11Buffer> ConstantBuffer (ID3D11Device* device, const std::vector<unsigned char>& bytes)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = UINT ((bytes.size () + 15u) & ~size_t (15u));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    std::vector<unsigned char> padded (desc.ByteWidth, 0);
    std::memcpy (padded.data (), bytes.data (), bytes.size ());
    D3D11_SUBRESOURCE_DATA initial = { padded.data (), 0, 0 };
    ComPtr<ID3D11Buffer> buffer;
    device->CreateBuffer (&desc, &initial, &buffer);
    return buffer;
}

inline std::vector<Float4> Run (const char* kernel, const Inputs& inputs, const std::vector<Float4>& input,
                                uint32_t outputsPerInput, std::string& error)
{
    namespace cs = geomsrv::archviz::dxgi::camerashader;
    std::vector<char> source (96 * 1024);
    const std::string body = geomsrv::archviz::dxgi::guestshaders::SceneBody () + kernel;
    if (!cs::Compose (inputs.interpretation, body.c_str (), source.data (), source.size ())) {
        error = "no declaration for the interpretation";
        return {};
    }
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED (D3DCompile (source.data (), std::strlen (source.data ()), "guest", nullptr, nullptr, "CSMain", "cs_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors))) {
        error = errors != nullptr ? (const char*) errors->GetBufferPointer () : "D3DCompile failed";
        return {};
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED (D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   nullptr, &context))) {
        error = "no WARP device";
        return {};
    }
    ComPtr<ID3D11ComputeShader> shader;
    device->CreateComputeShader (blob->GetBufferPointer (), blob->GetBufferSize (), nullptr, &shader);
    context->CSSetShader (shader.Get (), nullptr, 0);

    ComPtr<ID3D11ShaderReflection> reflection;
    D3DReflect (blob->GetBufferPointer (), blob->GetBufferSize (), IID_ID3D11ShaderReflection, (void**) &reflection);
    if (reflection == nullptr) {
        error = "no reflection";
        return {};
    }
    std::vector<ComPtr<ID3D11Buffer>> keep;
    auto bind = [&] (UINT slot, const std::vector<unsigned char>& bytes) {
        keep.push_back (ConstantBuffer (device.Get (), bytes));
        ID3D11Buffer* const buffer = keep.back ().Get ();
        context->CSSetConstantBuffers (slot, 1, &buffer);
    };
    auto matrix = [] (const float (&m)[16]) {
        std::vector<unsigned char> bytes (sizeof (m));
        std::memcpy (bytes.data (), m, sizeof (m));
        return bytes;
    };
    bind (1, matrix (inputs.view));
    bind (2, matrix (inputs.projection));
    // The guest's own buffers, those the kernel reads, each variable written by name.
    for (const char* name : { "GuestSceneFrame", "GuestDraw" }) {
        D3D11_SHADER_INPUT_BIND_DESC slot = {};
        if (FAILED (reflection->GetResourceBindingDescByName (name, &slot)))
            continue;
        ID3D11ShaderReflectionConstantBuffer* const buffer = reflection->GetConstantBufferByName (name);
        D3D11_SHADER_BUFFER_DESC described = {};
        buffer->GetDesc (&described);
        std::vector<unsigned char> bytes (described.Size, 0);
        for (const auto& [variable, values] : inputs.variables) {
            D3D11_SHADER_VARIABLE_DESC at = {};
            if (FAILED (buffer->GetVariableByName (variable.c_str ())->GetDesc (&at)))
                continue;
            std::memcpy (bytes.data () + at.StartOffset, values.data (),
                         (std::min) (values.size () * sizeof (float), size_t (at.Size)));
        }
        bind (slot.BindPoint, bytes);
    }

    const UINT count = UINT (input.size ());
    D3D11_BUFFER_DESC in = {};
    in.ByteWidth = count * sizeof (Float4);
    in.Usage = D3D11_USAGE_DEFAULT;
    in.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    in.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    in.StructureByteStride = sizeof (Float4);
    D3D11_SUBRESOURCE_DATA initial = { input.data (), 0, 0 };
    ComPtr<ID3D11Buffer> inputBuffer;
    device->CreateBuffer (&in, &initial, &inputBuffer);
    D3D11_BUFFER_DESC out = in;
    out.ByteWidth = outputsPerInput * count * sizeof (Float4);
    out.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Buffer> output;
    device->CreateBuffer (&out, nullptr, &output);
    D3D11_BUFFER_DESC staging = out;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.MiscFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> readback;
    device->CreateBuffer (&staging, nullptr, &readback);
    ComPtr<ID3D11ShaderResourceView> inputView;
    device->CreateShaderResourceView (inputBuffer.Get (), nullptr, &inputView);
    ComPtr<ID3D11UnorderedAccessView> outputView;
    device->CreateUnorderedAccessView (output.Get (), nullptr, &outputView);
    ID3D11ShaderResourceView* const srv = inputView.Get ();
    context->CSSetShaderResources (8, 1, &srv);
    ID3D11UnorderedAccessView* const uav = outputView.Get ();
    context->CSSetUnorderedAccessViews (0, 1, &uav, nullptr);
    context->Dispatch (count, 1, 1);
    context->CopyResource (readback.Get (), output.Get ());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED (context->Map (readback.Get (), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "the result could not be read";
        return {};
    }
    std::vector<Float4> result (size_t (outputsPerInput) * count);
    std::memcpy (result.data (), mapped.pData, result.size () * sizeof (Float4));
    context->Unmap (readback.Get (), 0);
    return result;
}

} // namespace guestkernel

#endif
