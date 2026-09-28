// ArchViz/Dxgi/OverlayShaderSources: the floor-plan overlay's HLSL compiles, for every
// entry point, and each vertex shader reads exactly the inputs its input layout
// supplies. It is compiled at run time on Archicad's device; without this a typo or a
// renamed semantic is first found by a live run as "NOT DRAWING".

#include "ArchViz/Dxgi/OverlayShaderSources.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace sh = geomsrv::archviz::dxgi::overlayshaders;

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
        { "VSPlanStroke", "vs_5_0" },
        { "PSPlanStroke", "ps_5_0" },
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
    };
    for (const auto& [entry, inputs] : expected) {
        ID3DBlob* blob = nullptr;
        ASSERT_EQ (Compile (sh::kPlan, entry, "vs_5_0", &blob), "") << entry;
        EXPECT_EQ (Inputs (blob), inputs) << entry;
        blob->Release ();
    }
}
