// ArchViz/DiligentShaders: the scene's HLSL compiles, and the section plane (issue #14 P2)
// reaches the rasterizer from every stage that feeds it, without breaking the linkage to the
// pixel shaders behind them.
//
// These are compiled at run time on Archicad's device; without this a typo, or an output added
// in the wrong place, is first found by a live run as a viewer that does not open. The linkage
// check is the D3D11 rule the section plane relies on: a pixel shader's inputs must be a PREFIX
// of the previous stage's outputs, so SV_ClipDistance0 has to be the LAST output everywhere.

#include "ArchViz/DiligentShaders.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <cstring>
#include <string>
#include <vector>

using namespace geomsrv::archviz;

namespace {

struct Compiled {
    std::string error;
    ID3DBlob* blob = nullptr;
    ~Compiled ()
    {
        if (blob != nullptr)
            blob->Release ();
    }
};

void Compile (const char* body, const char* target, Compiled& out)
{
    const std::string source = ArchVizShaderSource (body);
    ID3DBlob* errors = nullptr;
    const HRESULT hr =
        D3DCompile (source.c_str (), source.size (), "scene", nullptr, nullptr, "main", target,
                    D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out.blob, &errors);
    if (FAILED (hr) || out.blob == nullptr)
        out.error = errors != nullptr ? (const char*) errors->GetBufferPointer () : "D3DCompile failed";
    if (errors != nullptr)
        errors->Release ();
}

// "SEMANTICindex" for each input or output parameter, in signature order.
std::vector<std::string> Signature (ID3DBlob* blob, bool outputs)
{
    std::vector<std::string> names;
    ID3D11ShaderReflection* reflection = nullptr;
    if (blob == nullptr || FAILED (D3DReflect (blob->GetBufferPointer (), blob->GetBufferSize (),
                                               IID_ID3D11ShaderReflection, (void**) &reflection)))
        return names;
    D3D11_SHADER_DESC desc = {};
    reflection->GetDesc (&desc);
    const UINT count = outputs ? desc.OutputParameters : desc.InputParameters;
    for (UINT i = 0; i < count; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        if (outputs)
            reflection->GetOutputParameterDesc (i, &p);
        else
            reflection->GetInputParameterDesc (i, &p);
        names.push_back (std::string (p.SemanticName) + std::to_string (p.SemanticIndex));
    }
    reflection->Release ();
    return names;
}

bool HasClipDistance (const std::vector<std::string>& outputs)
{
    for (const std::string& name : outputs)
        if (_stricmp (name.c_str (), "SV_ClipDistance0") == 0)
            return true;
    return false;
}

void ExpectPrefix (const std::vector<std::string>& producer, const std::vector<std::string>& consumer, const char* what)
{
    ASSERT_LE (consumer.size (), producer.size ()) << what;
    for (size_t i = 0; i < consumer.size (); ++i)
        EXPECT_EQ (_stricmp (producer[i].c_str (), consumer[i].c_str ()), 0)
            << what << ": input " << i << " is " << consumer[i] << ", the producer's is " << producer[i];
}

} // namespace

TEST (SceneShaders, EveryTouchedStageCompiles)
{
    const struct {
        const char* name;
        const char* body;
        const char* target;
    } stages[] = {
        { "mesh VS", kArchVizMeshVS, "vs_5_0" },        { "shadow VS", kArchVizShadowVS, "vs_5_0" },
        { "outline VS", kArchVizOutlineVS, "vs_5_0" },  { "flat PS", kArchVizFlatPS, "ps_5_0" },
        { "G-buffer PS", kArchVizGBufferPS, "ps_5_0" }, { "wire VS", kArchVizWireVS, "vs_5_0" },
        { "wire HS", kArchVizWireHS, "hs_5_0" },        { "wire DS", kArchVizWireDS, "ds_5_0" },
        { "wire GS", kArchVizWireGS, "gs_5_0" },        { "wire PS", kArchVizWirePS, "ps_5_0" },
    };
    for (const auto& stage : stages) {
        Compiled c;
        Compile (stage.body, stage.target, c);
        EXPECT_TRUE (c.error.empty ()) << stage.name << ":\n" << c.error;
    }
}

TEST (SceneShaders, SectionPlaneReachesTheRasterizerFromEveryModelStage)
{
    Compiled mesh, shadow, outline, wireGs;
    Compile (kArchVizMeshVS, "vs_5_0", mesh);
    Compile (kArchVizShadowVS, "vs_5_0", shadow);
    Compile (kArchVizOutlineVS, "vs_5_0", outline);
    Compile (kArchVizWireGS, "gs_5_0", wireGs);
    EXPECT_TRUE (HasClipDistance (Signature (mesh.blob, true))) << "mesh VS (forward, G-buffer, AO, ids)";
    EXPECT_TRUE (HasClipDistance (Signature (shadow.blob, true))) << "shadow VS";
    EXPECT_TRUE (HasClipDistance (Signature (outline.blob, true))) << "outline VS";
    EXPECT_TRUE (HasClipDistance (Signature (wireGs.blob, true))) << "wire GS (wireframe, edges)";
}

TEST (SceneShaders, PixelShaderInputsStayPrefixesOfTheirProducers)
{
    Compiled mesh, outline, flat, gbuffer, wireDs, wireGs, wirePs;
    Compile (kArchVizMeshVS, "vs_5_0", mesh);
    Compile (kArchVizOutlineVS, "vs_5_0", outline);
    Compile (kArchVizFlatPS, "ps_5_0", flat);
    Compile (kArchVizGBufferPS, "ps_5_0", gbuffer);
    Compile (kArchVizWireDS, "ds_5_0", wireDs);
    Compile (kArchVizWireGS, "gs_5_0", wireGs);
    Compile (kArchVizWirePS, "ps_5_0", wirePs);
    ExpectPrefix (Signature (mesh.blob, true), Signature (flat.blob, false), "mesh VS -> flat PS");
    ExpectPrefix (Signature (mesh.blob, true), Signature (gbuffer.blob, false), "mesh VS -> G-buffer PS");
    ExpectPrefix (Signature (outline.blob, true), Signature (flat.blob, false), "outline VS -> flat PS");
    ExpectPrefix (Signature (wireGs.blob, true), Signature (wirePs.blob, false), "wire GS -> wire PS");
    ExpectPrefix (Signature (wireDs.blob, true), Signature (wireGs.blob, false), "wire DS -> wire GS");
}

// #14: the Technical white flash. With shadows off the shadow block is never mapped, and on D3D12
// an unmapped dynamic buffer reads a recycled page -- so every read of it in the mesh pass has to
// sit behind the "shadows live this frame" lane, g_shadowParams.z, or a stale bVisualizeShadowing
// paints every surface with the unshadowed 1.0.
TEST (SceneShaders, ShadowDiagnosticsReadTheShadowBlockOnlyWhileShadowsAreLive)
{
    const std::string tail = geomsrv::archviz::kArchVizMeshPSMainTail;
    for (const char* flag : { "g_shadowAttribs.bVisualizeShadowing", "g_shadowAttribs.bVisualizeCascades" }) {
        size_t at = tail.find (flag);
        ASSERT_NE (at, std::string::npos) << flag;
        for (; at != std::string::npos; at = tail.find (flag, at + 1)) {
            const size_t lineStart = tail.rfind ('\n', at) + 1;
            EXPECT_NE (tail.substr (lineStart, at - lineStart).find ("g_shadowParams.z > 0.5 &&"), std::string::npos)
                << flag << " is read without the shadows-live gate";
        }
    }
}

// #14: the Technical edges' geometry shader -- kArchVizWireGS built with ARCHVIZ_EDGE_FINS, a
// strip along each drawn edge instead of the triangle -- compiles, with the same outputs.
TEST (SceneShaders, TechnicalEdgeFinGeometryShaderCompiles)
{
    const std::string source = ArchVizShaderSource ("#define ARCHVIZ_EDGE_FINS 1\n", kArchVizWireGS);
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile (source.c_str (), source.size (), "fins", nullptr, nullptr, "main", "gs_5_0",
                                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    const std::string error = errors != nullptr ? (const char*) errors->GetBufferPointer () : "";
    EXPECT_TRUE (SUCCEEDED (hr) && blob != nullptr) << error;
    if (blob != nullptr)
        blob->Release ();
    if (errors != nullptr)
        errors->Release ();
}
