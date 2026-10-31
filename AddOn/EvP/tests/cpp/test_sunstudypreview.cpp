#include "ArchViz/SunStudyPreview.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/DiligentShaders.hpp"
#include "Palette/ParamCalendarValue.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <wrl/client.h>

using namespace geomsrv::archviz;

TEST (SunStudyPreview, DurationTooltipIsOnlyHoursAndMinutes)
{
    EXPECT_EQ (SunStudyClock (5.0, true), "5:00h");
    EXPECT_EQ (SunStudyClock (2.5, true), "2:30h");
    EXPECT_EQ (SunStudyClock (0.0, true), "0:00h");
    EXPECT_EQ (SunStudyClock (8.0), "8:00");
    EXPECT_EQ (SunStudyClock (24.0), "24:00");
}

TEST (SunStudyPreview, ClockRangeUsesAnExclusiveEndLikeTheCalculationInputs)
{
    const std::vector<uint16_t> minutes = { 480, 540, 600, 660, 720, 780, 840, 900, 960, 1020 };
    EXPECT_EQ (SunStudyPreviewSteps (minutes, 8.0f, 18.0f), std::make_pair (0u, 10u));
    EXPECT_EQ (SunStudyPreviewSteps (minutes, 8.0f, 17.0f), std::make_pair (0u, 9u));
    EXPECT_EQ (SunStudyPreviewSteps (minutes, 8.0f, 8.0f), std::make_pair (0u, 0u));
    EXPECT_EQ (SunStudyPreviewSteps (minutes, 8.25f, 16.75f), std::make_pair (1u, 9u));
    EXPECT_EQ (SunStudyPreviewSteps (minutes, 8.25f, 8.5f), std::make_pair (1u, 1u));
}

TEST (SunStudyPreview, BriefMorningAndLongEveningShadowsRemainDifferent)
{
    const std::vector<uint32_t> lit = { 0xfffffffEu & ~(0xFu << 8), 0xfffffffEu };
    EXPECT_FLOAT_EQ (SunStudyShadowFraction (lit, 0, 8), 0.125f);
    EXPECT_FLOAT_EQ (SunStudyShadowFraction (lit, 8, 16), 0.5f);
    EXPECT_FLOAT_EQ (SunStudyShadowFraction (lit, 30, 34), 0.25f);
    EXPECT_FLOAT_EQ (SunStudyShadowFraction (lit, 2, 2), 0.0f);
}

// The single shadow refines its boundary cells from the scene's shadow map
// (kArchVizSunTintShadowPS, which needs DiligentFX's Shadows.fxh). These tests
// check the tint's own arithmetic, so they stand in for that lookup with "no
// cascade covers the point" -- the shader's own fallback to the study cells.
// tools/quality/check_hlsl.py compiles the real composition.
constexpr const char* kNoShadowMapEdge = "float StudyEdgeLight (float3 worldPos, float3 normal) { return -1.0; }\n";

TEST (SunStudyPreview, ActualTintShaderCompilesWithAllPreviewConstants)
{
    using Microsoft::WRL::ComPtr;
    const std::string source = std::string (kArchVizCBuffer) + kNoShadowMapEdge + kArchVizSunTintPS;
    ComPtr<ID3DBlob> code, error;
    const HRESULT result = D3DCompile (source.data (), source.size (), "sun-tint", nullptr, nullptr, "main", "ps_5_0",
                                       D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &error);
    ASSERT_TRUE (SUCCEEDED (result)) << (error ? static_cast<const char*> (error->GetBufferPointer ())
                                               : "compile failed");
}

TEST (SunStudyPreview, RealShaderReadsBlueCutoffAndShadowDurationOnD3D)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE (SUCCEEDED (D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &device, nullptr, &context)));
    const std::string source = std::string (kArchVizCBuffer) + kNoShadowMapEdge + kArchVizSunTintPS + R"hlsl(
RWStructuredBuffer<float4> result : register(u0);
[numthreads(1,1,1)] void PreviewTest(uint3 id : SV_DispatchThreadID) {
    result[0] = StudyOverlayColor(SunRamp(2.0, 0.25));
    result[1] = StudyOverlayColor(SunRamp(2.5, 0.25));
    result[2] = float4(ShadowFraction(int2(0,0),0,8), ShadowFraction(int2(0,0),8,16), ShadowFraction(int2(0,0),30,34), 1.0);
    result[3] = float4(AmPmDurationColor(1.0,0.0), 1.0);
    result[4] = float4(AmPmDurationColor(0.0,1.0), 1.0);
    result[5] = float4(AmPmDurationColor(0.125,0.0), 1.0);
}
)hlsl";
    ComPtr<ID3DBlob> code, error;
    ASSERT_TRUE (SUCCEEDED (D3DCompile (source.data (), source.size (), "sun-preview", nullptr, nullptr, "PreviewTest",
                                        "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error)))
        << (error ? static_cast<const char*> (error->GetBufferPointer ()) : "compile failed");
    ComPtr<ID3D11ComputeShader> shader;
    ASSERT_TRUE (
        SUCCEEDED (device->CreateComputeShader (code->GetBufferPointer (), code->GetBufferSize (), nullptr, &shader)));
    DiligentSceneConstants values;
    values.sunStudyPreview[0] = 2.5f;
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof (values);
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = { &values, 0, 0 };
    ComPtr<ID3D11Buffer> constants;
    ASSERT_TRUE (SUCCEEDED (device->CreateBuffer (&constantsDesc, &constantsData, &constants)));
    const std::vector<uint32_t> lit = { 0xfffffffEu & ~(0xFu << 8), 0xfffffffEu };
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = textureDesc.Height = textureDesc.MipLevels = textureDesc.SampleDesc.Count = 1;
    textureDesc.ArraySize = 2;
    textureDesc.Format = DXGI_FORMAT_R32_UINT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA slices[2] = { { &lit[0], 4, 0 }, { &lit[1], 4, 0 } };
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> steps;
    ASSERT_TRUE (SUCCEEDED (device->CreateTexture2D (&textureDesc, slices, &texture)));
    ASSERT_TRUE (SUCCEEDED (device->CreateShaderResourceView (texture.Get (), nullptr, &steps)));
    // Reflect the real shader's slots, not guessed texture registers.
    ComPtr<ID3D11ShaderReflection> reflection;
    ASSERT_TRUE (
        SUCCEEDED (D3DReflect (code->GetBufferPointer (), code->GetBufferSize (), IID_PPV_ARGS (&reflection))));
    D3D11_SHADER_INPUT_BIND_DESC stepBinding = {};
    ASSERT_TRUE (SUCCEEDED (reflection->GetResourceBindingDescByName ("g_sunSteps", &stepBinding)));
    D3D11_BUFFER_DESC outputDesc = {};
    outputDesc.ByteWidth = 6 * 4 * sizeof (float);
    outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    outputDesc.StructureByteStride = 4 * sizeof (float);
    ComPtr<ID3D11Buffer> output, staging;
    ComPtr<ID3D11UnorderedAccessView> view;
    ASSERT_TRUE (SUCCEEDED (device->CreateBuffer (&outputDesc, nullptr, &output)));
    ASSERT_TRUE (SUCCEEDED (device->CreateUnorderedAccessView (output.Get (), nullptr, &view)));
    outputDesc.BindFlags = outputDesc.MiscFlags = 0;
    outputDesc.Usage = D3D11_USAGE_STAGING;
    outputDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ASSERT_TRUE (SUCCEEDED (device->CreateBuffer (&outputDesc, nullptr, &staging)));
    context->CSSetShader (shader.Get (), nullptr, 0);
    context->CSSetConstantBuffers (0, 1, constants.GetAddressOf ());
    context->CSSetShaderResources (stepBinding.BindPoint, 1, steps.GetAddressOf ());
    context->CSSetUnorderedAccessViews (0, 1, view.GetAddressOf (), nullptr);
    context->Dispatch (1, 1, 1);
    context->CopyResource (staging.Get (), output.Get ());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE (SUCCEEDED (context->Map (staging.Get (), 0, D3D11_MAP_READ, 0, &mapped)));
    const float* rgba = static_cast<const float*> (mapped.pData);
    const auto linear = [] (float value) { return std::pow ((value + 0.055f) / 1.055f, 2.4f); };
    // Below the cutoff: the solid low-sun category, kSunLowSunHex, with nothing of the warm ramp in it.
    EXPECT_NEAR (rgba[0], linear (float ((kSunLowSunHex >> 16) & 0xff) / 255.0f), 1e-6);
    EXPECT_NEAR (rgba[1], linear (float ((kSunLowSunHex >> 8) & 0xff) / 255.0f), 1e-6);
    EXPECT_NEAR (rgba[2], linear (float (kSunLowSunHex & 0xff) / 255.0f), 1e-6);
    EXPECT_FLOAT_EQ (rgba[3], 0.90f);
    EXPECT_FLOAT_EQ (rgba[7], 0.90f);
    EXPECT_NEAR (rgba[4], linear (0.612f), 1e-6) << "cutoff equality stays warm";
    EXPECT_FLOAT_EQ (rgba[8], SunStudyShadowFraction (lit, 0, 8));
    EXPECT_FLOAT_EQ (rgba[9], SunStudyShadowFraction (lit, 8, 16));
    EXPECT_FLOAT_EQ (rgba[10], SunStudyShadowFraction (lit, 30, 34));
    EXPECT_GT (rgba[14], rgba[12]) << "morning is purple";
    EXPECT_GT (rgba[16], rgba[18]) << "evening is salmon";
    EXPECT_GT (rgba[20], rgba[12]) << "a brief morning shadow is less intense than a full half-day";
    context->Unmap (staging.Get (), 0);
}

TEST (CalendarValue, RejectsMalformedAndNormalisedDates)
{
    int year = 0, month = 0, day = 0;
    EXPECT_TRUE (evp::ParseCalendarValue ("2028-02-29", year, month, day));
    EXPECT_EQ (year, 2028);
    EXPECT_EQ (month, 2);
    EXPECT_EQ (day, 29);
    for (const char* value :
         { "2026-02-29", "2026-2-03", "2026-12-32", "2026-13-01", "2038-01-01", "1901-12-31", "xxxx-03-21" })
        EXPECT_FALSE (evp::ParseCalendarValue (value, year, month, day)) << value;
}

TEST (SunStudyBands, DefaultEdgesSpreadEvenlyWithTheOpenBandAtNinePlus)
{
    float edges[kSunBandsMax - 1];
    SunStudyDefaultBandEdges (4, edges);
    EXPECT_FLOAT_EQ (edges[0], 3.0f);
    EXPECT_FLOAT_EQ (edges[1], 6.0f);
    EXPECT_FLOAT_EQ (edges[2], 9.0f);
    SunStudyDefaultBandEdges (3, edges);
    EXPECT_FLOAT_EQ (edges[0], 4.5f);
    EXPECT_FLOAT_EQ (edges[1], 9.0f);
    // Ten bands are exactly the smooth ramp's one-hour bins.
    SunStudyDefaultBandEdges (10, edges);
    for (int i = 0; i < 9; ++i) {
        EXPECT_FLOAT_EQ (edges[i], float (i + 1));
        EXPECT_FLOAT_EQ (SunStudyBandCentre (i, 10, edges), float (i) + 0.5f);
    }
    SunStudyDefaultBandEdges (12, edges);
    for (int i = 1; i < 11; ++i)
        EXPECT_GT (edges[i], edges[i - 1]);
    EXPECT_FLOAT_EQ (edges[10], 9.0f);
}

TEST (SunStudyBands, BoundariesAndTheOpenLastBand)
{
    float edges[kSunBandsMax - 1];
    SunStudyDefaultBandEdges (4, edges);
    EXPECT_EQ (SunStudyBand (0.0f, 4, edges), 0);
    EXPECT_EQ (SunStudyBand (2.75f, 4, edges), 0);
    EXPECT_EQ (SunStudyBand (3.0f, 4, edges), 1);
    EXPECT_EQ (SunStudyBand (2.9999f, 4, edges), 1) << "a 10-minute study's 3:00 h sits a few ulps under the edge";
    EXPECT_EQ (SunStudyBand (8.75f, 4, edges), 2);
    EXPECT_EQ (SunStudyBand (9.0f, 4, edges), 3);
    EXPECT_EQ (SunStudyBand (14.0f, 4, edges), 3) << "the last band is open-ended";
    EXPECT_FLOAT_EQ (SunStudyBandCentre (0, 4, edges), 1.5f);
    EXPECT_FLOAT_EQ (SunStudyBandCentre (3, 4, edges), 9.5f);
}

TEST (SunStudyBands, ClampKeepsDraggedEdgesOrderedOnTheQuarterHourGrid)
{
    float edges[kSunBandsMax - 1] = { 5.1f, 2.0f, 12.0f };
    SunStudyClampBandEdges (4, edges);
    EXPECT_FLOAT_EQ (edges[0], 5.0f);
    EXPECT_FLOAT_EQ (edges[1], 5.25f);
    EXPECT_FLOAT_EQ (edges[2], 9.75f);
}

// The legend (SunStudyBand / SunStudyBandCentre) and the tint (SunBand /
// SunBandCentre) must name the same band and the same colour.
TEST (SunStudyBands, RealShaderSteppedRampMatchesTheLegendAndTheOverlayIsOpaque)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE (SUCCEEDED (D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &device, nullptr, &context)));
    const float probes[8] = { 0.0f, 2.75f, 3.0f, 4.5f, 6.25f, 8.9f, 9.0f, 15.0f };
    std::string source = std::string (kArchVizCBuffer) + kNoShadowMapEdge + kArchVizSunTintPS +
                         "RWStructuredBuffer<float4> result : register(u0);\n"
                         "[numthreads(1,1,1)] void BandTest(uint3 id : SV_DispatchThreadID) {\n";
    for (int i = 0; i < 8; ++i)
        source += "    result[" + std::to_string (i) + "] = float4(SunRamp(" + std::to_string (probes[i]) +
                  ", 0.25), float(SunBand(" + std::to_string (probes[i]) + ", 4)));\n";
    source += "    result[8] = HoursOverlayColor(float3(0.5, 0.5, 0.5), float3(0,0,0), float3(0,0,1));\n}\n";
    ComPtr<ID3DBlob> code, error;
    ASSERT_TRUE (SUCCEEDED (D3DCompile (source.data (), source.size (), "sun-bands", nullptr, nullptr, "BandTest",
                                        "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error)))
        << (error ? static_cast<const char*> (error->GetBufferPointer ()) : "compile failed");
    ComPtr<ID3D11ComputeShader> shader;
    ASSERT_TRUE (
        SUCCEEDED (device->CreateComputeShader (code->GetBufferPointer (), code->GetBufferSize (), nullptr, &shader)));
    DiligentSceneConstants values;
    float edges[kSunBandsMax - 1];
    SunStudyDefaultBandEdges (4, edges);
    values.sunStudyPreview[3] = 4.0f;
    std::copy (edges, edges + 11, values.sunStudyBands);
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof (values);
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = { &values, 0, 0 };
    ComPtr<ID3D11Buffer> constants;
    ASSERT_TRUE (SUCCEEDED (device->CreateBuffer (&constantsDesc, &constantsData, &constants)));
    D3D11_BUFFER_DESC outputDesc = {};
    outputDesc.ByteWidth = 9 * 4 * sizeof (float);
    outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    outputDesc.StructureByteStride = 4 * sizeof (float);
    ComPtr<ID3D11Buffer> output, staging;
    ComPtr<ID3D11UnorderedAccessView> view;
    ASSERT_TRUE (SUCCEEDED (device->CreateBuffer (&outputDesc, nullptr, &output)));
    ASSERT_TRUE (SUCCEEDED (device->CreateUnorderedAccessView (output.Get (), nullptr, &view)));
    outputDesc.BindFlags = outputDesc.MiscFlags = 0;
    outputDesc.Usage = D3D11_USAGE_STAGING;
    outputDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ASSERT_TRUE (SUCCEEDED (device->CreateBuffer (&outputDesc, nullptr, &staging)));
    context->CSSetShader (shader.Get (), nullptr, 0);
    context->CSSetConstantBuffers (0, 1, constants.GetAddressOf ());
    context->CSSetUnorderedAccessViews (0, 1, view.GetAddressOf (), nullptr);
    context->Dispatch (1, 1, 1);
    context->CopyResource (staging.Get (), output.Get ());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE (SUCCEEDED (context->Map (staging.Get (), 0, D3D11_MAP_READ, 0, &mapped)));
    const float* rgba = static_cast<const float*> (mapped.pData);
    // The ramp's own sRGB stops (kSunBins, red channel), decoded: what the
    // legend's swatch at the band centre shows.
    const float bins[10] = { 0.420f, 0.541f, 0.612f, 0.690f, 0.753f, 0.812f, 0.863f, 0.906f, 0.941f, 0.969f };
    const auto linear = [] (float c) { return c <= 0.04045f ? c / 12.92f : std::pow ((c + 0.055f) / 1.055f, 2.4f); };
    const auto warmRed = [&] (float hours) {
        const float t = std::clamp (hours - 0.5f, 0.0f, 9.0f);
        const int lo = int (std::floor (t)), hi = (std::min) (lo + 1, 9);
        return linear (bins[lo] + (bins[hi] - bins[lo]) * (t - float (lo)));
    };
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ (int (rgba[4 * i + 3]), SunStudyBand (probes[i], 4, edges)) << probes[i] << " h";
        // SunRamp classifies the hours AFTER snapping them to the timestep, as
        // the smooth ramp does: 8.9 h in a 15-minute study is 9:00 h, the open band.
        const float snapped = std::floor (probes[i] / 0.25f + 0.5f) * 0.25f;
        const int band = SunStudyBand (snapped, 4, edges);
        EXPECT_NEAR (rgba[4 * i], warmRed (SunStudyBandCentre (band, 4, edges)), 1e-5) << probes[i] << " h";
    }
    EXPECT_FLOAT_EQ (rgba[4 * 2], rgba[4 * 3]) << "one flat colour across a band";
    EXPECT_FLOAT_EQ (rgba[35], 1.0f) << "the hours view is opaque: no sun shadow shows through";
    context->Unmap (staging.Get (), 0);
}

namespace {
class SunStudyControls : public testing::Test {
  protected:
    ImGuiContext* context = nullptr;
    float from = 8.0f, to = 17.0f, threshold = 2.5f;
    int bands = 0;
    float edges[11] = { 3.0f, 6.0f, 9.0f };
    ImVec2 start, size;
    int inspect = 2;
    float inspectBottom = 0.0f;
    void SetUp () override
    {
        context = ImGui::CreateContext ();
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2 (500, 300);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault ();
    }
    void TearDown () override
    {
        ImGui::DestroyContext (context);
    }
    void Frame (float x, bool down, bool gradient = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (x, 30.0f);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos (ImVec2 (0, 0));
        ImGui::SetNextWindowSize (ImVec2 (320, 200));
        ImGui::Begin ("controls", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorPosY (20);
        start = ImGui::GetCursorScreenPos ();
        size = ImVec2 (ImGui::GetContentRegionAvail ().x, ImGui::GetFrameHeight ());
        if (gradient)
            DrawSunStudyGradient (true, threshold, bands, bands > 0 ? edges : nullptr);
        else
            DrawSunStudyRange ("range", from, to, 0.0f, 24.0f, false);
        ImGui::End ();
        ImGui::Render ();
    }
    void InspectFrame (float x, bool down, const char* first = "", const char* second = "")
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (x, 48.0f);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos (ImVec2 (0, 0));
        ImGui::SetNextWindowSize (ImVec2 (320, 250));
        ImGui::Begin ("controls", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorPosY (20);
        start = ImGui::GetCursorScreenPos ();
        size = ImVec2 (ImGui::GetContentRegionAvail ().x, ImGui::GetFrameHeight ());
        DrawSunStudyInspectControl (inspect, first, second);
        inspectBottom = ImGui::GetCursorPosY ();
        ImGui::End ();
        ImGui::Render ();
    }
};
} // namespace

TEST_F (SunStudyControls, TwoHandlesMoveIndependentlyAndCannotCross)
{
    Frame (-1, false);
    const auto x = [&] (float hour) { return start.x + 7 + (size.x - 14) * hour / 24.0f; };
    Frame (x (8), false);
    Frame (x (8), true);
    Frame (x (10), true);
    EXPECT_FLOAT_EQ (from, 10.0f);
    EXPECT_FLOAT_EQ (to, 17.0f);
    Frame (x (10), false);
    Frame (x (17), false);
    Frame (x (17), true);
    Frame (x (20), true);
    EXPECT_FLOAT_EQ (to, 20.0f);
    EXPECT_FLOAT_EQ (from, 10.0f);
    Frame (x (3), true);
    EXPECT_FLOAT_EQ (to, from);
}

TEST_F (SunStudyControls, BlueBoxDragsOnTheGradientInQuarterHours)
{
    Frame (-1, false, true);
    const auto x = [&] (float hour) { return start.x + size.x * hour / 10.0f; };
    Frame (x (2.5f), false, true);
    Frame (x (2.5f), true, true);
    Frame (x (2.0f), true, true);
    EXPECT_FLOAT_EQ (threshold, 2.0f);
}

TEST_F (SunStudyControls, PanelReadingKeepsTheSameSpaceForEveryHoverState)
{
    InspectFrame (-1, false);
    const float empty = inspectBottom;
    InspectFrame (-1, false, "5.00 h direct sun", "5.75 h shadow of 10.75 h daylight");
    EXPECT_FLOAT_EQ (inspectBottom, empty);
    InspectFrame (-1, false, "cursor ray missed the hovered element");
    EXPECT_FLOAT_EQ (inspectBottom, empty);
    InspectFrame (-1, false, "context: casts shadow, not measured");
    EXPECT_FLOAT_EQ (inspectBottom, empty);
}

TEST_F (SunStudyControls, ThreePositionHoverButtonsSelectOffCursorAndPanelDirectly)
{
    InspectFrame (-1, false);
    for (const int wanted : { 0, 1, 2 }) {
        const float x = start.x + size.x * (float (wanted) + 0.5f) / 3.0f;
        InspectFrame (x, false);
        InspectFrame (x, true);
        InspectFrame (x, false);
        EXPECT_EQ (inspect, wanted);
    }
}

TEST_F (SunStudyControls, LowSunIsASolidSwatchOnTheGradientNotATranslucentWash)
{
    Frame (-1, false, true);
    Frame (-1, false, true);
    bool solid = false, translucent = false;
    const ImU32 blue = IM_COL32 ((kSunLowSunHex >> 16) & 0xff, (kSunLowSunHex >> 8) & 0xff, kSunLowSunHex & 0xff, 255);
    const auto* data = ImGui::GetDrawData ();
    for (int list = 0; list < data->CmdListsCount; ++list)
        for (const auto& vertex : data->CmdLists[list]->VtxBuffer) {
            solid = solid || vertex.col == blue;
            translucent = translucent || vertex.col == IM_COL32 (40, 83, 107, 153);
        }
    EXPECT_TRUE (solid);
    EXPECT_FALSE (translucent);
}

TEST_F (SunStudyControls, SteppedGripDragsAnEdgeBetweenItsNeighbours)
{
    bands = 4;
    Frame (-1, false, true);
    const auto x = [&] (float hour) { return start.x + size.x * hour / 10.0f; };
    Frame (x (6.0f), false, true);
    Frame (x (6.0f), true, true);
    Frame (x (7.5f), true, true);
    EXPECT_FLOAT_EQ (edges[1], 7.5f);
    Frame (x (9.9f), true, true);
    EXPECT_FLOAT_EQ (edges[1], 8.75f) << "held a quarter hour short of the next edge";
    EXPECT_FLOAT_EQ (edges[0], 3.0f);
    EXPECT_FLOAT_EQ (edges[2], 9.0f);
    EXPECT_FLOAT_EQ (threshold, 2.5f) << "the blue box did not move";
}
