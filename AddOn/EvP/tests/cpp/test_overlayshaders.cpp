// ArchViz/Dxgi/OverlayShaderSources: the overlays' HLSL compiles, for every entry point
// and every camera slot, and each vertex shader reads exactly the inputs its input
// layout supplies. These are compiled at run time on Archicad's device; without this a
// typo or a renamed semantic is first found by a live run as "NOT DRAWING".

#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/GuestShaderSources.hpp"
#include "ArchViz/Dxgi/OverlayShaderSources.hpp"
#include "ArchViz/Dxgi/SurfaceFocusContext.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace sh = geomsrv::archviz::dxgi::overlayshaders;
namespace cs = geomsrv::archviz::dxgi::camerashader;

namespace {

// The run time's flags; an empty string is success, anything else the compiler's words.
std::string Compile (const char* source, const char* entry, const char* target, ID3DBlob** keep = nullptr)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile (source, std::strlen (source), "overlay", nullptr, nullptr, entry, target,
                                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    std::string message;
    if (FAILED (hr) || blob == nullptr)
        message = errors != nullptr ? (const char*) errors->GetBufferPointer () : "D3DCompile failed";
    if (errors != nullptr)
        errors->Release ();
    if (keep != nullptr && message.empty ())
        *keep = blob;
    else if (blob != nullptr)
        blob->Release ();
    return message;
}

// The vertex inputs a shader reads, as "SEMANTIC<index>", system values excluded.
std::vector<std::string> Inputs (ID3DBlob* blob)
{
    std::vector<std::string> inputs;
    ID3D11ShaderReflection* reflection = nullptr;
    if (FAILED (D3DReflect (blob->GetBufferPointer (), blob->GetBufferSize (), IID_ID3D11ShaderReflection,
                            (void**) &reflection)))
        return inputs;
    D3D11_SHADER_DESC desc = {};
    reflection->GetDesc (&desc);
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter = {};
        reflection->GetInputParameterDesc (i, &parameter);
        if (parameter.SystemValueType != D3D_NAME_UNDEFINED)
            continue;
        inputs.push_back (std::string (parameter.SemanticName) + std::to_string (parameter.SemanticIndex));
    }
    reflection->Release ();
    return inputs;
}

} // namespace

TEST (OverlayShaders, EveryFloorPlanEntryPointCompiles)
{
    const std::pair<const char*, const char*> entries[] = {
        { "VSPlanStroke", "vs_5_0" }, { "PSPlanStroke", "ps_5_0" }, { "VSLayerStroke", "vs_5_0" },
        { "VSLayerFill", "vs_5_0" },  { "PSLayer", "ps_5_0" },
    };
    for (const auto& [entry, target] : entries)
        EXPECT_EQ (Compile (sh::kPlan, entry, target), "") << entry;
}

// ⚠️ THE LAYOUTS ARE BUILT IN C++ AND THE SHADERS READ THEM BY NAME. A semantic that
// drifts on either side is a CreateInputLayout failure on Archicad's device.
TEST (OverlayShaders, EachFloorPlanVertexShaderReadsItsLayout)
{
    const std::pair<const char*, std::vector<std::string>> expected[] = {
        { "VSPlanStroke", { "SEGMENT0", "SEGMENT1" } },
        { "VSLayerStroke", { "SEGMENT0", "SEGMENT1", "COLOR0", "WIDTH0" } },
        { "VSLayerFill", { "POSITION0", "POSITION1", "COLOR0" } },
    };
    for (const auto& [entry, inputs] : expected) {
        ID3DBlob* blob = nullptr;
        ASSERT_EQ (Compile (sh::kPlan, entry, "vs_5_0", &blob), "") << entry;
        EXPECT_EQ (Inputs (blob), inputs) << entry;
        blob->Release ();
    }
}

// ---- the Diligent guest (GuestShaderSources.hpp) ----------------------------------

namespace {

namespace guest = geomsrv::archviz::dxgi::guestshaders;

std::vector<std::string> Attributes (size_t count)
{
    std::vector<std::string> names;
    for (size_t i = 0; i < count; ++i)
        names.push_back ("ATTRIB" + std::to_string (i));
    return names;
}

} // namespace

TEST (OverlayShaders, EveryGuestPlanEntryPointCompiles)
{
    const std::string source = guest::PlanSource ();
    const std::pair<const char*, const char*> entries[] = {
        { "VSFill", "vs_5_0" }, { "VSLine", "vs_5_0" }, { "VSGlyph", "vs_5_0" },
        { "PSFill", "ps_5_0" }, { "PSLine", "ps_5_0" }, { "PSGlyph", "ps_5_0" },
    };
    for (const auto& [entry, target] : entries)
        EXPECT_EQ (Compile (source.c_str (), entry, target), "") << entry;
}

// ⚠️ DILIGENT NUMBERS THE LAYOUT, THE SHADER NAMES IT ATTRIBn. Each count is the
// field count of the OverlayScene struct the guest's input layout describes.
TEST (OverlayShaders, EachGuestPlanVertexShaderReadsItsLayout)
{
    const std::string source = guest::PlanSource ();
    const std::pair<const char*, size_t> expected[] = {
        { "VSFill", 5 },  // PlanFillVertex: hi, lo, offset, rgba, value
        { "VSLine", 7 },  // PlanLine: hiA, loA, hiB, loB, rgba, style, dashes
        { "VSGlyph", 10 } // PlanGlyph: hi, lo, offset, uv, dir, rgba, halo, haloPixels, flags, minSpan
    };
    for (const auto& [entry, count] : expected) {
        ID3DBlob* blob = nullptr;
        ASSERT_EQ (Compile (source.c_str (), entry, "vs_5_0", &blob), "") << entry;
        EXPECT_EQ (Inputs (blob), Attributes (count)) << entry;
        blob->Release ();
    }
}

TEST (OverlayShaders, TheGuestSceneCompilesForEveryCameraSlot)
{
    const std::string body = guest::SceneBody ();
    std::vector<char> source (64 * 1024);
    const std::pair<const char*, size_t> expected[] = {
        { "VSFill", 5 },   // SceneFillVertex: position, normal, offset, rgba, value
        { "VSLine", 7 },   // SceneLine: a, b, rgba, hidden rgba, style, dashes, behind
        { "VSGlyph", 10 }, // SceneGlyph: includes the shared whole-label visibility bound
    };
    for (uint32_t slot = 0; slot < cs::kShaderSlots; ++slot) {
        ASSERT_TRUE (cs::Compose (cs::InterpretationOfSlot (slot), body.c_str (), source.data (), source.size ()))
            << slot;
        for (const auto& [entry, count] : expected) {
            ID3DBlob* blob = nullptr;
            ASSERT_EQ (Compile (source.data (), entry, "vs_5_0", &blob), "") << entry << " slot " << slot;
            EXPECT_EQ (Inputs (blob), Attributes (count)) << entry << " slot " << slot;
            blob->Release ();
        }
    }
    ASSERT_TRUE (cs::Compose (0, body.c_str (), source.data (), source.size ()));
    for (const char* entry : { "PSFill", "PSLine", "PSGlyph" })
        EXPECT_EQ (Compile (source.data (), entry, "ps_5_0"), "") << entry;
}

TEST (OverlayShaders, SurfaceFocusContextCompiles)
{
    namespace context = geomsrv::archviz::dxgi::surfacefocuscontext;
    EXPECT_EQ (Compile (context::kVertexShader, "VSSurfaceContext", "vs_5_0"), "");
    EXPECT_EQ (Compile (context::kPixelShader, "PSSurfaceContext", "ps_5_0"), "");
}

TEST (OverlayShaders, SurfaceFocusContextRestoresTheSceneViewportOnWarp)
{
    using Microsoft::WRL::ComPtr;
    namespace context = geomsrv::archviz::dxgi::surfacefocuscontext;
    context::Shutdown ();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> gpu;
    ASSERT_TRUE (SUCCEEDED (D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &device, nullptr, &gpu)));

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 16;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ASSERT_TRUE (SUCCEEDED (device->CreateTexture2D (&desc, nullptr, &texture)));
    ASSERT_TRUE (SUCCEEDED (device->CreateRenderTargetView (texture.Get (), nullptr, &target)));
    const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    gpu->ClearRenderTargetView (target.Get (), red);

    const D3D11_VIEWPORT incoming = { 3.0f, 2.0f, 7.0f, 5.0f, 0.0f, 1.0f };
    gpu->RSSetViewports (1, &incoming);
    ASSERT_EQ (context::Draw (gpu.Get (), target.Get (), 0.38f), context::DrawResult::Drawn);
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT restored[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    gpu->RSGetViewports (&count, restored);
    ASSERT_EQ (count, 1u);
    EXPECT_FLOAT_EQ (restored[0].TopLeftX, incoming.TopLeftX);
    EXPECT_FLOAT_EQ (restored[0].TopLeftY, incoming.TopLeftY);
    EXPECT_FLOAT_EQ (restored[0].Width, incoming.Width);
    EXPECT_FLOAT_EQ (restored[0].Height, incoming.Height);
    EXPECT_FLOAT_EQ (restored[0].MinDepth, incoming.MinDepth);
    EXPECT_FLOAT_EQ (restored[0].MaxDepth, incoming.MaxDepth);

    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE (SUCCEEDED (device->CreateTexture2D (&desc, nullptr, &staging)));
    gpu->OMSetRenderTargets (0, nullptr, nullptr);
    gpu->CopyResource (staging.Get (), texture.Get ());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE (SUCCEEDED (gpu->Map (staging.Get (), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto* pixel = static_cast<const uint8_t*> (mapped.pData);
    // Red's 0.2126 luminance fills every channel, then fades 62% toward 0.60.
    EXPECT_NEAR (pixel[0], 116, 2);
    EXPECT_NEAR (pixel[1], 116, 2);
    EXPECT_NEAR (pixel[2], 116, 2);
    EXPECT_EQ (pixel[3], 255);
    gpu->Unmap (staging.Get (), 0);

    // White model: red's luminance lifts to lerp (0.55, 1, 0.2126) = 0.6457, then
    // fades 62% toward white: 1 - 0.38 * 0.3543 = 0.8654.
    gpu->ClearRenderTargetView (target.Get (), red);
    ASSERT_EQ (context::Draw (gpu.Get (), target.Get (), 0.38f, true), context::DrawResult::Drawn);
    gpu->OMSetRenderTargets (0, nullptr, nullptr);
    gpu->CopyResource (staging.Get (), texture.Get ());
    ASSERT_TRUE (SUCCEEDED (gpu->Map (staging.Get (), 0, D3D11_MAP_READ, 0, &mapped)));
    pixel = static_cast<const uint8_t*> (mapped.pData);
    EXPECT_NEAR (pixel[0], 221, 2);
    EXPECT_NEAR (pixel[1], 221, 2);
    EXPECT_NEAR (pixel[2], 221, 2);
    EXPECT_EQ (pixel[3], 255);
    gpu->Unmap (staging.Get (), 0);
    context::Shutdown ();
}

TEST (OverlayShaders, TheThreeDLayerCompilesForEveryCameraSlot)
{
    char biased[cs::kMaxSource] = {};
    std::snprintf (biased, sizeof (biased), "static const float DepthBias = %.8f;\n%s", 0.00075, sh::kLayer3DBody);
    char source[cs::kMaxSource] = {};
    for (uint32_t slot = 0; slot < cs::kShaderSlots; ++slot) {
        ASSERT_TRUE (cs::Compose (cs::InterpretationOfSlot (slot), biased, source, sizeof (source))) << slot;
        ID3DBlob* blob = nullptr;
        ASSERT_EQ (Compile (source, "VSLayer", "vs_5_0", &blob), "") << "slot " << slot;
        EXPECT_EQ (Inputs (blob), (std::vector<std::string> { "POSITION0", "COLOR0" })) << slot;
        blob->Release ();
    }
    ASSERT_TRUE (cs::Compose (0, biased, source, sizeof (source)));
    EXPECT_EQ (Compile (source, "PSLayer", "ps_5_0"), "");
}
